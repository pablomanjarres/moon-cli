# Seguridad de archivos mapeados con mmap en moon

Sistemas Operativos | Parcial 3: Seguridad de Archivos en Capa Kernel y mmap

Pablo Manjarres

## Objetivo y uso

El módulo `kernel/mmap_guard.ko` protege un archivo: niega mapearlo con escritura
compartida o con ejecución. El comando `m_fmap` de moon mapea un archivo y muestra
cada llamada al sistema.

```text
make && make -C kernel
sudo insmod kernel/mmap_guard.ko path=/tmp/moon_secret.txt
./moon
m_fmap /tmp/moon_secret.txt r      # lectura compartida: permitida
m_fmap /tmp/moon_secret.txt w      # escritura compartida: mmap -> -1 EACCES
sh tests/run_mmap_guard_tests.sh   # ataques, registro y medición
```

Los modos son: `r` lectura, `w` escritura compartida, `p` copia privada, `x`
ejecución y `u` lectura ampliada con `mprotect()`.

## Parte I: preguntas diagnósticas

**1. De mmap() al primer fallo de página.** `vm_mmap_pgoff()` llama a
`security_mmap_file()`. Luego `do_mmap()` crea un `vm_area_struct` con `vm_file` y
`vm_ops`, sin crear PTE. En el primer acceso, la MMU no encuentra la PTE y lanza
`#PF`. `handle_mm_fault()` valida `vm_flags`. Después, `filemap_fault()` trae la
página del page cache e instala la PTE. Desde ahí los accesos no pasan por el
kernel, así que el control debe hacerse al crear la VMA.

**2. MAP_SHARED frente a MAP_PRIVATE.** `MAP_SHARED` agrega `VM_SHARED`: las PTE
apuntan a la página del page cache. Las escrituras llegan al disco y otros procesos
las ven. `MAP_PRIVATE` usa COW: la primera escritura crea una copia anónima del
proceso. Con un descriptor `O_RDONLY`, el kernel quita `VM_MAYWRITE | VM_SHARED`
(`mm/mmap.c:464`).

**3. Copy-On-Write.** La página empieza compartida y sin permiso de escritura. Al
escribir, `do_wp_page()` la copia y deja la copia escribible. Si el kernel confunde
una página COW con una escribible, la escritura llega al archivo. Eso pasó con
Dirty COW (CVE-2016-5195). Se escribía en `/proc/self/mem` mientras otro hilo
descartaba la copia con `MADV_DONTNEED`. Así se modificaban archivos de root, como
`/etc/passwd`.

**4. Tabla de syscalls frente a LSM.** Desde 5.7, `kallsyms_lookup_name` no está
exportado. Además, CR0.WP está fijado, así que modificar `sys_call_table` es frágil
e inseguro. El gancho LSM `mmap_file` (antes `file_mmap`) está en el camino de todas
las rutas, pero el LSM debe compilarse con el kernel. La opción elegida usa
kretprobes sobre `security_mmap_file()` y `security_file_mprotect()`. Ve lo mismo
que un LSM y se carga como módulo.

**5. Cifrado transparente.** El descifrado debe estar en el sistema de archivos,
debajo del page cache, como hace fscrypt en `->read_folio`. Solo quien tiene la
clave puede abrir el archivo. El aislamiento de tablas de páginas impide que otros
procesos lean la memoria. Contra ataques físicos se usa cifrado de RAM con
SME/SEV o TME.

**6. Páginas sucias y msync.** Escribir por el puntero solo marca sucia la PTE. El
writeback, `msync()` o `fsync()` las llevan al disco. Un filtro sobre `write()` no
ve esos cambios. Por eso el módulo impide crear el mapeo escribible.

**7. eBPF.** Sin recompilar, un programa en `kprobe`/`fexit` sobre
`security_mmap_file` o el tracepoint `page_fault_user` puede registrar pid, inode y
`prot`. BPF LSM (`lsm/mmap_file`, `CONFIG_BPF_LSM=y`) también puede bloquear. El
límite es que no ve los accesos después del primer fallo.

**8. O_RDONLY con PROT_WRITE y MAP_PRIVATE.** Se permite porque la escritura queda
en una copia privada y nunca llega al archivo. El riesgo es que el proceso altere
su propia vista. Si COW falla, como en Dirty COW, esto se vuelve escritura real.

**9. Carreras con MADV_DONTNEED.** El parche de Dirty COW agregó `FOLL_COW`. Desde
6.1, `can_follow_write_pte()` (`mm/gup.c:785`) solo permite escribir con
`FOLL_FORCE` en páginas anónimas exclusivas. Además, `mmap_lock` y el lock de la
tabla de páginas ordenan las dos operaciones. La prueba de carrera confirma que el
archivo no cambia.

**10. struct page, tablas de páginas y bits.** `struct page` indica si una página es
del page cache o anónima. La MMU recorre PGD→P4D→PUD→PMD→PTE y verifica los bits:
R/W bloquea escrituras, U/S bloquea el acceso a memoria del kernel y NX bloquea la
ejecución. Un mapeo sin `PROT_EXEC` tiene NX, por eso el módulo niega `PROT_EXEC`.

## Parte II: estrategia con apoyo de IA

### Flujo de trabajo

La IA se usó como asistente de un ingeniero, no como fuente de verdad. Cada
afirmación sobre el kernel se verificó con el código fuente de la versión probada.

- **Exploración.** La IA comparó caminos de intercepción: tabla de syscalls, LSM,
  kprobes, eBPF y `vm_ops->fault`. También identificó las funciones por donde pasan
  todas las rutas. Las firmas y las líneas citadas se verificaron en el árbol de
  Linux 6.18.
- **Código C.** La IA generó el primer borrador del módulo y de `m_fmap`. Se revisó
  cada símbolo: que exista, que sea sondeable y que use la API actual
  (`regs_get_kernel_argument`, `module_param_cb`). Se compiló con `-Wall` y con
  Kbuild sin advertencias.
- **Pruebas y PoC.** La IA propuso los vectores: mapeo compartido escribible,
  `MAP_SHARED_VALIDATE`, ampliación con `mprotect`, ejecución y carrera Dirty COW.
  Cada prueba compara el archivo con `read()` antes y después. No se acepta solo el
  código de retorno.

### Prompts estructurados

| # | Contexto | Rol | Restricción | Resultado esperado |
|---|---|---|---|---|
| 1 | Enunciado del parcial y repositorio moon con dos parciales previos | Arquitecto de kernel Linux | Solución simple, compatible con 5.x/6.x, módulo cargable y sin parchear el kernel | Comparación de puntos de intercepción y elección justificada |
| 2 | Kernel 6.18 WSL2, `CONFIG_KRETPROBES=y`, `CONFIG_MODVERSIONS=y` | Programador de módulos | Usar solo símbolos sondeables o exportados, sin CR0, sin `sys_call_table` y sin advertencias | `mmap_guard.c` con política, registro y contadores |
| 3 | Contrato `ui_trace_*` de moon y plantilla de `cat_data.c` | Desarrollador del shell | Mismo estilo de los comandos existentes y compilación en Linux y macOS | Comando `m_fmap` con traza de cada syscall |
| 4 | Política: denegar escritura compartida y ejecución | Analista de pruebas ofensivas | Usuario sin privilegios con permiso POSIX de escritura y verificación por contenido | `mmap_attacks.c`, carrera Dirty COW y prueba sin módulo como control |
| 5 | Resultados de la medición | Revisor de rendimiento | Medir solo lo que se puede reproducir en la máquina | Microbenchmark mmap+fallo+munmap con y sin módulo |

### Alucinaciones detectadas y corrección

| Sugerencia | Problema | Verificación |
|---|---|---|
| Sobrescribir `sys_call_table` obtenida con `kallsyms_lookup_name()` | No está exportada desde 5.7 | `kernel/kallsyms.c`: no tiene `EXPORT_SYMBOL` |
| Desactivar la protección con `write_cr0(read_cr0() & ~X86_CR0_WP)` | El kernel fija CR0.WP y lo restaura. Además, es inseguro con varias CPU | `arch/x86/kernel/cpu/common.c:422` |
| Registrar un LSM desde el módulo con `security_add_hooks()` | Es `__init`; los LSM se compilan dentro del kernel | `security/security.c:629` |
| Gancho `file_mmap` con firma `(file, reqprot, prot, flags, addr, addr_only)` | Es la firma de 3.x. Ahora es `mmap_file(file, reqprot, prot, flags)`, y el wrapper sondeado recibe `(file, prot, flags)` | `include/linux/lsm_hook_defs.h:202`, `security/security.c:3132` |
| Usar `VM_MAYSHARE` para decidir si `mprotect` escribe en el archivo | Un `MAP_SHARED` sobre `O_RDONLY` conserva `VM_MAYSHARE` sin `VM_SHARED`. Se usa `VM_SHARED` | `mm/mmap.c:464` |

### Validación manual

1. Se descargó el árbol `linux-msft-wsl-6.18.54.1` del kernel en ejecución. Se aplicó
   su `/proc/config.gz` y se compiló para obtener `Module.symvers`, porque
   `CONFIG_MODVERSIONS=y`.
2. Se buscó con `grep` cada símbolo, firma y bandera usado en el módulo.
3. Se compiló el módulo sin advertencias y se cargó y descargó con `insmod`/`rmmod`.
   Se verificó `dmesg` en ambos casos.
4. Se ejecutaron los ataques sin el módulo como grupo de control y con el módulo.

### Arquitectura

```text
 Ring 3   proceso: mmap(fd, PROT_WRITE, MAP_SHARED)      mprotect(addr, PROT_WRITE)
 -------------------------- syscall ----------------------------------------------
 Ring 0   vm_mmap_pgoff()                                do_mprotect_pkey()
              |                                               |
          security_mmap_file()  <-- kretprobe -->  security_file_mprotect()
              |        mmap_guard: ¿inode protegido?          |
              |        sí + (escritura compartida | exec) -> -EACCES
              |        no -> 0 (solo una comparación de puntero)
          do_mmap() -> VMA                         mprotect_fixup()
              |
          #PF -> handle_mm_fault() -> filemap_fault() -> PTE   (sin cambios)
```

### Matriz de compensaciones

| Opción | Latencia de mmap/fallos | Cobertura | Cargable sin recompilar | Riesgo |
|---|---|---|---|---|
| `LD_PRELOAD` | Ninguna | Nula: se evita con syscall directa | Sí | Bajo |
| Hook en `sys_call_table` | Baja | Parcial: no cubre caminos internos | No en 6.x | Alto: bloqueado por CR0/CFI |
| LSM compilado | Mínima | Total en creación y cambio de permisos | No | Bajo |
| BPF LSM | Mínima | Igual a LSM | Sí, requiere `bpf` en `lsm=` | Bajo |
| **kretprobe en ganchos LSM (elegida)** | Baja: una comparación por mmap | Igual a LSM para mmap/mprotect | Sí | Medio: depende de símbolos sondeables |
| Instrumentar `handle_mm_fault` | Alta: cada fallo | Por página, solo el primer acceso | Sí | Alto |

## Parte III: implementación y argumentación

### Código

- `kernel/mmap_guard.c` resuelve el inode de `path` al cargar y registra dos
  kretprobes. El manejador de entrada compara el inode. Si no coincide, devuelve 1 y
  no se ejecuta el de retorno. Para el archivo protegido niega `PROT_EXEC` y
  `PROT_WRITE` con mapeo compartido, y permite lo demás. El manejador de retorno
  cambia el 0 del gancho por `-EACCES` antes de crear la VMA o modificarla. Cada
  decisión se registra en `dmesg` con pid, uid, comm y `prot`, y se cuenta en
  `/sys/module/mmap_guard/parameters/{allowed,denied}`.
- `kernel/Makefile` usa Kbuild. El `Makefile` principal sigue compilando moon igual.
- `cat_memory.c` contiene `m_fmap`.
- `tests/mmap_attacks.c` prueba los ataques. `tests/mmap_bench.c` mide el costo y
  `tests/run_mmap_guard_tests.sh` ejecuta todo.

### Por qué en el kernel

Después del primer fallo, un acceso por puntero no ejecuta ninguna instrucción del
kernel. Una biblioteca en espacio de usuario solo puede envolver `mmap()`, y
cualquier binario estático o `syscall(SYS_mmap)` la evita. Los ganchos LSM están en
el único punto obligatorio: crear la VMA o ampliar sus permisos. Negar ahí impide
que se instale una PTE escribible. Por eso no hace falta inspeccionar cada
escritura, que sería imposible a la velocidad del hardware.

### Resultados

`KDIR=... KCC=gcc-13 sh tests/run_mmap_guard_tests.sh` se ejecutó en Ubuntu 26.04
sobre WSL2, con kernel 6.18.54.1-microsoft-standard-WSL2 y 6 núcleos. El árbol del
kernel se compiló con gcc 13, el mismo compilador del kernel en ejecución. Con
gcc 15, `insmod` rechazó el módulo porque `struct module` tenía otro tamaño.

Las 18 comprobaciones pasaron. El módulo compila sin advertencias. Sin el módulo, el
mismo usuario sí puede crear el mapeo compartido escribible: es el grupo de control.
Con el módulo:

- Se permiten la lectura compartida, la copia privada y la lectura compartida sobre un
  descriptor `O_RDWR`. Los bytes mapeados coinciden con `read()`.
- Se rechazan con `EACCES` la escritura compartida (`MAP_SHARED` y
  `MAP_SHARED_VALIDATE`), la ejecución y las ampliaciones con `mprotect` a escritura
  o ejecución.
- En la carrera Dirty COW, el kernel aceptó 153 736 escrituras por `/proc/self/mem`.
  Todas cayeron en la copia privada y el archivo quedó idéntico byte a byte.
- `m_fmap w` muestra `mmap -> -1 EACCES`. `dmesg` registra cada decisión y los
  contadores terminaron en `allowed=6 denied=6`.
- Al descargar el módulo, la escritura compartida vuelve a funcionar.

Medición con 200000 iteraciones de `mmap` de una página, primer acceso y `munmap`:

| Archivo | Sin módulo (ns, mediana de 5) | Con módulo (ns, mediana de 5) | Diferencia |
|---|---|---|---|
| Protegido (entrada completa del probe) | 8560 | 8445 | -1.3 % |
| Otro archivo (solo comparación de inode) | 8809 | 8976 | +1.9 % |

Cada iteración produjo un fallo de página menor, igual con y sin módulo. Por lo
tanto, el módulo no cambia la tasa de fallos: actúa antes de crear la VMA, no en
`handle_mm_fault`. Las diferencias son menores que la variación entre corridas,
de 7771 a 8883 ns. En esta máquina, el sobrecosto no se distingue del ruido. Se
midió con `clock_gettime` y `getrusage`, porque el kernel de WSL no trae `perf` ni
`strace`. El costo esperado es el de un kprobe por `mmap` y `mprotect`; las lecturas
y escrituras por puntero no tienen costo adicional.

### Limitaciones y trabajo futuro

- **Escrituras normales.** El módulo protege mmap. Las escrituras con `write()`
  siguen sujetas a los permisos POSIX. Un tercer gancho en
  `security_file_permission` cerraría ese camino a costa de una comparación por cada
  `read`/`write` del sistema.
- **Mapeos previos.** Un mapeo creado antes de cargar el módulo sigue vigente.
- **Root.** Root puede descargar el módulo. Con `lockdown=integrity` y módulos
  firmados, o con el mismo código como BPF LSM fijado al arranque, se evita.
- **kprobes.** Se pueden desactivar o pueden cambiar los nombres de los símbolos. Un
  LSM compilado o BPF LSM es más estable.
- **Un archivo.** Se protege un solo archivo, identificado por inode. Si se renombra
  el archivo, sigue protegido. Si se reemplaza por otro inode, no.
- **Memoria física.** Un proceso con acceso a la RAM, DMA o un hipervisor
  comprometido ve los datos en claro. Mitigaciones: IOMMU para DMA, Intel TME/AMD SME
  para cifrar la RAM y SEV-SNP/TDX para aislar la VM del hipervisor.
