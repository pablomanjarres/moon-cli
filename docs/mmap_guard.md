# Seguridad de archivos mapeados con mmap en moon

Sistemas Operativos | Parcial 3: Seguridad de Archivos en Capa Kernel y mmap

Pablo Manjarres

## Objetivo y uso

El módulo `kernel/mmap_guard.ko` protege un archivo: niega mapearlo con escritura
compartida o con ejecución, también cuando se intenta con `mprotect()`. El comando
`m_fmap` de moon mapea un archivo y muestra cada llamada al sistema. Requiere Linux,
gcc, make, sudo y el árbol de compilación del kernel en ejecución.

```text
make && make -C kernel                 # moon y mmap_guard.ko
sudo insmod kernel/mmap_guard.ko path=/tmp/moon_secret.txt
./moon
m_fmap /tmp/moon_secret.txt r          # lectura compartida: permitida
m_fmap /tmp/moon_secret.txt w          # escritura compartida: mmap -> -1 EACCES
sh tests/run_mmap_guard_tests.sh       # ataques, registro y contadores
sh tests/mmap_overhead.sh              # sobrecosto con ftrace y strace
sh kernel/demo.sh                      # demostración paso a paso
```

Los modos de `m_fmap` son: `r` lectura, `w` escritura compartida, `p` copia privada,
`x` ejecución y `u` lectura ampliada con `mprotect()`.

## Parte I: preguntas diagnósticas

**1. De mmap() al primer fallo de página.** `mmap()` entra a Ring 0 por `syscall`:
`ksys_mmap_pgoff()` → `vm_mmap_pgoff()`. Allí `security_mmap_file()` consulta los
LSM. Con `mmap_lock` tomado, `do_mmap()` calcula `vm_flags` y `mmap_region()` crea un
`vm_area_struct` con `vm_file`, `vm_pgoff` y los `vm_ops` del sistema de archivos. No
se crea ninguna PTE. En el primer acceso, la MMU recorre PGD→P4D→PUD→PMD→PTE, no
encuentra una entrada presente y lanza `#PF` con la dirección en CR2.
`exc_page_fault()` llama a `handle_mm_fault()`, que valida el acceso contra
`vm_flags` y llama a `do_fault()`. En una lectura, `filemap_fault()` trae la página
del page cache, o del disco, e instala una PTE de solo lectura. En una escritura
privada, `do_cow_fault()` copia la página. En una escritura compartida,
`do_shared_fault()` avisa al sistema de archivos con `->page_mkwrite`. Desde ahí, los
accesos no pasan por el kernel. Por eso el control debe hacerse al crear la VMA.

**2. MAP_SHARED frente a MAP_PRIVATE.** `do_mmap()` traduce `MAP_SHARED` a
`VM_SHARED | VM_MAYSHARE`: las PTE apuntan a la misma página del page cache que ven
`read()` y los demás procesos. `MAP_PRIVATE` no lleva `VM_SHARED`: la primera
escritura hace COW y crea una página anónima del proceso. Con un descriptor
`O_RDONLY`, el kernel quita `VM_MAYWRITE | VM_SHARED` (`mm/mmap.c:464`), así que el
mapeo nunca puede pasar a escritura. En seguridad, con `MAP_SHARED` una escritura
persiste en disco y todos los procesos la ven de inmediato, así que un proceso
comprometido corrompe los datos de todos. Con `MAP_PRIVATE`, los cambios no persisten
ni se comparten. Pero la copia queda en memoria anónima del proceso, que puede ir a
swap o a un volcado de memoria.

**3. Copy-On-Write.** La página empieza compartida con el page cache y con una PTE
sin escritura, aunque la VMA tenga `VM_WRITE`. La primera escritura lanza `#PF`.
`do_wp_page()` comprueba que la VMA permite escribir, copia la página a una anónima
nueva, instala la PTE escribible y suelta la referencia a la original. El peligro
aparece cuando el kernel concede escritura donde solo hay COW. Pasaría si `mprotect`
aceptara `PROT_WRITE` sin `VM_MAYWRITE`, o si `get_user_pages()` con `FOLL_FORCE`
escribiera en la página del page cache en vez de en la copia. Eso fue Dirty COW
(CVE-2016-5195): se escribía en `/proc/self/mem` mientras otro hilo descartaba la
copia con `MADV_DONTNEED`. Así, un usuario sin privilegios modificaba archivos de
solo lectura, como `/etc/passwd` o binarios SUID, y obtenía root.

**4. Tabla de syscalls frente a LSM.** Un LKM con hook en `sys_call_table` se carga
sin recompilar y ve cada syscall al entrar. Pero `kallsyms_lookup_name()` no está
exportado desde 5.7, la tabla es de solo lectura y el bit CR0.WP está fijado desde
5.3. Además, no ve las rutas internas del kernel y lee argumentos crudos de usuario,
que pueden cambiar después de revisarlos (TOCTOU). Su viabilidad en 6.x es muy baja.

Un LSM usa los ganchos `mmap_file` (llamado `file_mmap` antes de 3.5) y
`file_mprotect`. Está en el camino de todas las rutas, recibe `struct file`, `prot` y
`flags` ya validados, y es la API soportada. Su desventaja es que hay que compilarlo
con el kernel, porque `security_add_hooks()` es `__init`. BPF LSM evita recompilar,
pero requiere `bpf` en `lsm=`. Elegí kretprobes sobre `security_mmap_file()` y
`security_file_mprotect()`: ven lo mismo que un LSM y se cargan como módulo.

**5. Cifrado transparente.** El descifrado va debajo del page cache, en el sistema de
archivos. fscrypt descifra en `->read_folio` y deja el texto claro en el page cache.
Al escribir, cifra en una página temporal durante el writeback, sin tocar la página
en caché. El acceso se controla al abrir el archivo: sin la clave en el keyring no
se llega al contenido. Otro proceso no puede leer esa página, porque solo aparece en
las tablas de páginas de quien la mapea, y `/proc/pid/mem` exige
`ptrace_may_access()`. Al retirar la clave, fscrypt expulsa las páginas en claro.
Contra la lectura física de la RAM o por DMA se usan cifrado de memoria (AMD SME/SEV,
Intel TME) e IOMMU.

**6. Páginas sucias, msync y fsync.** En un mapeo compartido escribible, la PTE
empieza sin permiso de escritura. La primera escritura a una página limpia produce un
fallo: `do_shared_fault()` llama a `->page_mkwrite` y marca la página sucia
(`folio_mark_dirty`). Después deja la PTE escribible, con el bit dirty que pone la
MMU. El writeback escribe las páginas sucias con `->writepages`. Antes, con
`folio_mkclean()`, quita otra vez la escritura de las PTE para detectar la próxima
escritura. `msync(MS_SYNC)` y `fsync()` inician ese writeback y esperan a que
termine. `munmap()` y la reclamación de memoria también pasan el bit dirty de la PTE
a la página.

Así se evaden los filtros: un filtro que inspecciona `write()` no ve las escrituras
por puntero. Además, `page_mkwrite` solo avisa en la primera escritura sobre una
página limpia. Los bytes siguientes cambian sin aviso hasta el próximo writeback, así
que un atacante puede escribir después de la inspección. Por eso el módulo impide
crear el mapeo compartido escribible.

**7. Auditoría con eBPF.** Sin recompilar, bpftrace o libbpf cargan programas en
kprobes y `fentry`/`fexit`. Se pueden poner sobre `security_mmap_file`,
`security_file_mprotect` y `handle_mm_fault`, o en los tracepoints
`syscalls:sys_enter_mmap` y `exceptions:page_fault_user`. El programa guarda pid,
uid, inode, `prot` y dirección en mapas BPF o en un ring buffer. Con eso detecta
patrones anómalos, como un proceso que mapea con escritura un inode sensible o un
número inusual de fallos de escritura por segundo. El verificador garantiza que el
programa no rompa el kernel. Para bloquear, BPF LSM (`lsm/mmap_file`, compilado en el
kernel probado) devuelve `-EPERM`. Límite: después del primer fallo, los accesos por
puntero no generan eventos.

**8. O_RDONLY con PROT_WRITE y MAP_PRIVATE.** Sí se permite. `do_mmap()` solo exige
`FMODE_WRITE` para `MAP_SHARED` con `PROT_WRITE`. Con `MAP_PRIVATE`, la escritura cae
en una copia COW anónima y nunca llega al archivo. Implica tres riesgos:

- **Alteración temporal.** El proceso cambia su propia vista hasta `munmap`, por
  ejemplo una configuración o un código ya cargados.
- **Fuga de información.** Los datos quedan en páginas anónimas que pueden ir a swap
  o a un volcado de memoria.
- **Cambios ajenos.** Las páginas aún no escritas siguen mostrando lo que otros
  escriban en el archivo.

Si COW falla, como en Dirty COW, esto se vuelve una escritura real. El módulo permite
este caso y la prueba confirma que el archivo no cambia.

**9. Carreras con MADV_DONTNEED.** El parche de Dirty COW (commit 19be0eaffa3a,
Linux 4.9) dejó de quitar `FOLL_WRITE` al reintentar y agregó `FOLL_COW`. En 6.0,
`FOLL_COW` se eliminó (commit 5535be309971). Desde entonces, `can_follow_write_pte()`
(`mm/gup.c:785`) solo permite escribir con `FOLL_FORCE` si la página es anónima y
exclusiva de esa VMA (`PageAnonExclusive`), nunca en una del page cache.
`madvise(MADV_DONTNEED)` y GUP pueden correr en paralelo con `mmap_lock` en lectura.
El lock de la tabla de páginas serializa cada cambio de PTE, y si la PTE desaparece,
GUP vuelve a resolver el fallo con una copia nueva. Desde 6.12,
`proc_mem.force_override` (`fs/proc/base.c:126`) permite prohibir del todo esas
escrituras forzadas.

**10. struct page, tablas de páginas y bits de la MMU.** Cada marco físico tiene un
`struct page` o folio con un contador de referencias, `mapping` (page cache o
anónima) y banderas. Con ellos, el kernel decide si una escritura va a la página
compartida o a una copia. La MMU traduce cada dirección con PGD→P4D→PUD→PMD→PTE y
verifica en hardware los bits de cada nivel. R/W impide escribir páginas de solo
lectura, U/S impide que Ring 3 acceda a las páginas del kernel y NX impide ejecutar
páginas de datos. SMEP y SMAP impiden que el kernel ejecute o lea memoria de usuario por error. Un
archivo mapeado sin `PROT_EXEC` tiene NX en sus PTE, así que el código inyectado ahí
produce `#PF`. Para que ese control no se pueda desactivar, el módulo niega
`PROT_EXEC` y su ampliación con `mprotect()`.

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

## Parte III: implementación y resultados

El módulo resuelve el inode de `path` al cargar. En cada `mmap` o `mprotect`
compara ese inode. Si el archivo es el protegido y la petición pide `PROT_EXEC` o
escritura compartida, cambia la respuesta a `-EACCES`. Registra cada decisión en
`dmesg` y la cuenta en `/sys/module/mmap_guard/parameters/{allowed,denied}`. Debe
estar en el kernel porque, después del primer fallo, los accesos por puntero no
pasan por ninguna biblioteca. Además, una syscall directa evita `LD_PRELOAD`.

**Pruebas** (Ubuntu 26.04 en WSL2, kernel 6.18.54.1): pasaron las 18
comprobaciones. Sin el módulo, el mismo usuario sí puede escribir el archivo por
mmap. Con el módulo:

- Se permiten la lectura y la copia privada.
- Se rechazan con `EACCES` la escritura compartida, `MAP_SHARED_VALIDATE`, la
  ejecución y las ampliaciones con `mprotect`.
- En la carrera Dirty COW, el kernel aceptó 153 736 escrituras por
  `/proc/self/mem`. Todas quedaron en la copia privada y el archivo no cambió.
- `m_fmap w` muestra `mmap -> -1 EACCES`.

**Sobrecosto** (`mmap` + primer acceso + `munmap`, mediana de 5 corridas de 200000):

| Archivo | Sin módulo | Con módulo |
|---|---|---|
| Protegido | 8560 ns | 8445 ns |
| Otro | 8809 ns | 8976 ns |

La diferencia es menor al 2 % y queda dentro del ruido entre corridas. El número de
fallos de página no cambia, porque el módulo no toca `handle_mm_fault`. Medí con
`clock_gettime` y `getrusage` porque WSL no trae `perf`.

**Limitaciones:**

- `write()` sigue dependiendo de los permisos POSIX. Se podría cubrir con un
  gancho en `security_file_permission`.
- Los mapeos creados antes de cargar el módulo siguen activos.
- Root puede descargar el módulo. Para evitarlo se necesita `lockdown` o BPF LSM
  desde el arranque.
- El módulo solo protege un archivo.
- No protege contra acceso físico a la RAM o DMA. Para eso se usan IOMMU, TME/SME y
  SEV-SNP/TDX.
