# Compresor Huffman concurrente en moon

Sistemas Operativos | Parcial 2: Concurrencia y Sincronización

Pablo Manjarres y Valentina Barbosa

## Objetivo y uso

La alternativa 1 agrega compresión y descompresión Huffman en segundo plano al
editor del Parcial 1. La implementación usa C, POSIX/Linux y sincronización
explícita con pthreads, mutex y variables de condición.

Compilar con `make` y ejecutar `./moon`. `edit archivo.txt` abre la vista visual;
`Ctrl+L` permite ejecutar las órdenes. `edit` sin archivo abre el modo de líneas.

```text
c archivo.huf
u archivo.huf recuperado.txt
status
cancel
```

`c` comprime el archivo guardado; `u` descomprime a un archivo nuevo.
`status` muestra el progreso y `cancel` solicita la cancelación del trabajo.
Las rutas con espacios se escriben entre comillas.

## Hilos y sincronización

`huffman.c` concentra la copia de entrada, el algoritmo y los recursos del trabajo.
Un hilo coordinador captura el archivo en una copia temporal. Cuatro trabajadores
leen bloques de 64 KiB y cuentan frecuencias en tablas locales. La reducción se
protege con el mutex; `pthread_join` separa esta fase de la construcción del árbol
global, que queda inmutable para la codificación paralela.

Una ventana de ocho resultados limita la memoria pendiente. El mutex protege
el estado, la asignación de bloques y esa ventana. Las variables de condición
suspenden a los trabajadores cuando está llena y al coordinador cuando falta el
siguiente bloque. El coordinador escribe por índice, aunque los hilos terminen
en otro orden. La descompresión también procesa bloques en paralelo.

## Integración con el editor

Solo el hilo del editor accede al documento y al terminal. Una tubería de
notificaciones y `poll` actualizan el progreso mientras se espera entrada,
sin espera activa. Antes de `c`, los cambios del cursor se guardan con `Ctrl+O`.
Durante la captura se permite escribir, pero los cambios en disco se rechazan
temporalmente. Al terminar la captura, se puede editar y guardar mientras los
trabajadores usan la copia inmutable.

## Integridad y recursos

El formato `MOONHF01` guarda tamaño original, frecuencias, checksum FNV-1a de
64 bits y longitudes por bloque. La descompresión verifica estructura, bits,
frecuencias y checksum antes de publicar el resultado. Conserva los bytes,
incluidos valores nulos y archivos sin salto final.

La salida se escribe en un temporal y se publica mediante `link`, que rechaza
destinos existentes. La publicación y el estado completo comparten el mutex;
una cancelación posterior no cambia un resultado ya publicado. Cancelar o salir
despierta a los hilos, los reúne, cierra los descriptores y elimina los temporales.

## Verificación y límites

Ejecutar `make && sh tests/run_editor_tests.sh`. La suite pasó en Fedora/Linux y
macOS: archivos vacíos, un símbolo, binarios, varios bloques, entradas corruptas,
salidas idénticas con uno y cuatro hilos, edición durante la compresión y limpieza
al cancelar o salir. ASan, UBSan y ThreadSanitizer pasaron; la comprobación de
memoria reportó cero fugas.

La copia requiere espacio temporal en disco. La cabecera puede aumentar el
tamaño de archivos pequeños. La vista visual del editor admite texto ASCII.
