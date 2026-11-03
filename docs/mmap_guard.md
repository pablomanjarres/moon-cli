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

## Parte II: estrategia con IA

Usé la IA para comparar puntos de intercepción, escribir el primer borrador del
módulo y proponer ataques. Todo lo que dijo sobre el kernel lo verifiqué en el
código fuente de Linux 6.18.

| Contexto | Rol | Restricción | Resultado esperado |
|---|---|---|---|
| Enunciado y repositorio moon | Arquitecto de kernel | Módulo cargable, sin parchear el kernel | Comparar puntos de intercepción |
| Kernel 6.18 con kretprobes | Programador de módulos | Sin `sys_call_table`, sin CR0, sin advertencias | `mmap_guard.c` |
| Contrato `ui_trace_*` de moon | Desarrollador del shell | Mismo estilo de los comandos existentes | Comando `m_fmap` |
| Política del módulo | Pruebas ofensivas | Usuario sin privilegios y verificación del contenido | `mmap_attacks.c` y carrera Dirty COW |

**Alucinaciones detectadas:**

| Sugerencia | Por qué falla | Dónde se verificó |
|---|---|---|
| Usar `kallsyms_lookup_name()` para hallar `sys_call_table` | No está exportado desde 5.7 | `kernel/kallsyms.c` |
| Quitar CR0.WP con `write_cr0` | El kernel fija y restaura el bit | `arch/x86/kernel/cpu/common.c:422` |
| Registrar un LSM con `security_add_hooks()` desde el módulo | Es `__init` | `security/security.c:629` |
| `file_mmap` con firma de 6 argumentos | La firma es de 3.x | `include/linux/lsm_hook_defs.h:202` |

**Validación:** compilé el árbol del kernel en uso con su `/proc/config.gz` y gcc 13.
Con gcc 15, `insmod` rechazó el módulo por un `struct module` de otro tamaño. Luego
cargué y descargué el módulo, revisé `dmesg` y ejecuté los ataques con y sin módulo.

**Arquitectura:**

```text
 Ring 3   mmap(fd, PROT_WRITE, MAP_SHARED)       mprotect(addr, PROT_WRITE)
 ------------------------------ syscall -------------------------------------
 Ring 0   security_mmap_file()   <- kretprobe ->  security_file_mprotect()
              mmap_guard: ¿inode protegido y (escritura compartida o exec)?
              sí -> -EACCES      no -> sigue normal (una comparación)
          do_mmap() -> VMA -> #PF -> filemap_fault() -> PTE   (sin cambios)
```

| Opción | Costo | Cobertura | Sin recompilar |
|---|---|---|---|
| `LD_PRELOAD` | Ninguno | Nula: se evita con una syscall directa | Sí |
| Hook en `sys_call_table` | Bajo | Parcial y bloqueado en 6.x | No |
| LSM o BPF LSM | Mínimo | Total | No (o requiere `lsm=bpf`) |
| **kretprobe en ganchos LSM** | Una comparación por mmap | Igual a LSM | Sí |
| Instrumentar `handle_mm_fault` | Alto: cada fallo | Solo el primer acceso | Sí |

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
