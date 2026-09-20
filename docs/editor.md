# Editor de texto en `moon`

Trabajo de Sistemas Operativos de Pablo Manjarres y Valentina Barbosa. El código
está en `cat_editor.c`; se entra desde el shell con `edit`.

## Uso

`edit` abre el modo de líneas. `edit archivo.txt` abre el mismo archivo en una
vista de pantalla completa. En esa vista, `Ctrl+O` guarda y `Ctrl+X` sale.

| Modo de líneas | Qué hace | Llamadas usadas |
|---|---|---|
| `o archivo` | Abre o crea el archivo | `open`, `close` al cambiar de archivo |
| `p [n]` | Imprime todo o la línea `n` | `lseek`, `read`, `write` |
| `a texto` | Añade una línea | `lseek`, `read`, `write` |
| `d n` | Borra la línea `n` | `lseek`, `read`, `write`, `ftruncate` |
| `q` | Cierra el archivo y vuelve a `moon` | `close` |
| `i n texto` | Inserta una línea en `n` | `lseek`, `read`, `write`, `ftruncate` |
| `s palabra` | Muestra las líneas que contienen la palabra | `lseek`, `read` |

## Integración con el shell

`main.c` ya despacha comandos desde `commands[]` en `registry.c`. Añadimos
`edit` a esa tabla y declaramos `cmd_edit` en `shell.h`. No hubo que cambiar
el ciclo principal ni el parser.

La categoría `data` agrupa operaciones que terminan tras una llamada. El
editor toma la entrada hasta que el usuario sale y mantiene abierto un
descriptor entre órdenes. Por eso tiene su propia categoría:

```c
/* antes: última categoría */
{ "util", "Utilities", "getuid · time" },

/* ahora: una fila más en categories[] */
{ "editor", "Text editor", "open · read · write · lseek · ftruncate · termios" },
```

`q` retorna de `cmd_edit` después de cerrar el descriptor. No llama a
`exit()`: hacerlo también cerraría el shell que contiene al editor.

## Archivos y memoria

El modo de líneas guarda el descriptor en `ed_fd`. Lee el archivo completo
para localizar líneas y, después de `d` o `i`, escribe el resultado desde el
inicio y ajusta su longitud con `ftruncate`. En pantalla completa, `Doc`
guarda un arreglo dinámico de líneas; `Ctrl+O` las reúne y escribe el archivo.
`tcgetattr` y `tcsetattr` permiten leer teclas una a una y restaurar el
terminal al salir.

Se eligió cargar el texto en memoria porque simplifica las operaciones por
línea. Editar solo en disco usaría menos RAM, pero complicaría mover la cola
del archivo. Un índice de offsets evitaría releer todo para `p n`, pero
habría que actualizarlo tras cada inserción o borrado. Aquí se pagan lecturas
y escrituras completas, una decisión razonable para archivos pequeños.

```text
d 2: [uno\n][dos\n][tres\n]  ->  [uno\n][tres\n]
              destino <---- origen   memmove admite el solapamiento

i 2: [uno\n][tres\n] + [dos\n] -> [uno\n][dos\n][tres\n]
      se copia a otro búfer; origen y destino no se solapan
```

## Errores y límites

Las llamadas de archivo y las reservas de memoria comprueban sus retornos;
los fallos de sistema se muestran con `perror`. Si falla `o` con otro nombre,
el archivo anterior sigue abierto. Se usan `open`, `read`, `write`, `lseek`,
`ftruncate` y `close` para el archivo, sin `fopen`, `fread`, `fwrite` ni
`fclose`.

El archivo debe caber en memoria. El guardado reescribe el archivo en su
lugar, sin copia temporal, así que no es atómico. La vista completa acepta
teclas imprimibles ASCII; no es un editor Unicode. Los cambios de esa vista
se guardan solo con `Ctrl+O`.

## Pruebas

En Linux: `make && sh tests/run_editor_tests.sh`. El script comprueba los
comandos de ambos niveles, fallos de apertura, archivos sin salto final y
edición visual mediante una terminal seudográfica.
