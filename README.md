# Simulador de Ejecución de Procesos (Round-Robin)

Programa en C estándar que simula la planificación de procesos a nivel de sistema operativo. Lee un archivo de texto con la configuración del sistema (procesadores e hilos) y la lista de procesos, y simula su ejecución con la política **Round-Robin** sobre una cola FIFO, mostrando el estado de la simulación en consola.

## Contenido del proyecto

| Archivo         | Descripción                                          |
|-----------------|------------------------------------------------------|
| `simulador.c`   | Código fuente completo del simulador (un solo archivo) |
| `simulador`     | Ejecutable compilado para Linux                      |
| `simulador.exe`     | Ejecutable compilado para Windows                      |
| `config.txt`    | Archivo de prueba con datos de ejemplo               |
| `README.md`     | Este documento                                       |

## Requisitos

- Compilador de C compatible con C99 (GCC, Clang o MSVC).
- No depende de librerías externas. Solo usa la librería estándar de C; la pausa entre ciclos usa `nanosleep` en Unix/Linux/macOS y `Sleep` en Windows.

## Compilación

**Linux / macOS**

```bash
gcc -std=c99 -Wall -Wextra -o simulador simulador.c
```

**Windows (GCC / MinGW)**

```bash
gcc -std=c99 -o simulador.exe simulador.c
```

**Windows (MSVC, Developer Command Prompt)**

```bat
cl simulador.c
```

## Ejecución

```bash
./simulador [archivo_config] [retardo_ms]
```

| Argumento        | Descripción                                                                 | Por defecto  |
|------------------|-----------------------------------------------------------------------------|--------------|
| `archivo_config` | Ruta del archivo de entrada                                                 | `config.txt` |
| `retardo_ms`     | Pausa entre ciclos en milisegundos (0 a 60000). Use `0` para no tener pausas | `300`        |

Ejemplos:

```bash
./simulador                    # usa config.txt con 300 ms entre ciclos
./simulador config.txt 500     # pausa de 500 ms entre ciclos
./simulador config.txt 0       # sin pausas (útil para redirigir la salida a un archivo)
./simulador config.txt 0 > salida.txt
```

## Formato del archivo de entrada

```text
Procesadores N
Hilos K

ID | IDPadre | ProgramCounter | Registros | Tamano | Hilos | Quantum | Iteracion
```

- Las líneas vacías y las que empiezan con `#` se ignoran.
- `Procesadores` y `Hilos` deben aparecer **antes** de los procesos (las palabras clave no distinguen mayúsculas de minúsculas).

| Campo            | Descripción                                           | Validación                  |
|------------------|-------------------------------------------------------|-----------------------------|
| `Procesadores`   | Cantidad de procesadores simulados                    | 1 a 64                      |
| `Hilos`          | Cantidad de hilos del sistema                         | 1 a 1024                    |
| `ID`             | Identificador único del proceso                       | Entero > 0, sin duplicados  |
| `IDPadre`        | Proceso padre (0 si es el proceso inicial)            | Entero ≥ 0                  |
| `ProgramCounter` | Nombre o identificador del proceso                    | Texto no vacío              |
| `Registros`      | Cantidad de registros que utiliza                     | Entero ≥ 0                  |
| `Tamano`         | Tamaño del proceso en bytes                           | Entero ≥ 0                  |
| `Hilos`          | Hilos asociados al proceso                            | Entero > 0                  |
| `Quantum`        | Pulsos de procesador que usa en cada turno            | Entero > 0                  |
| `Iteracion`      | Veces que se ejecuta el proceso (turnos totales)      | Entero > 0                  |

### Ejemplo

```text
Procesadores 2
Hilos 4

1 | 0 | Proceso1 | 5 | 1024 | 2 | 10 | 3
2 | 1 | Proceso2 | 4 | 512  | 1 | 8  | 2
3 | 0 | Proceso3 | 6 | 2048 | 3 | 12 | 4
```

## Modelo de simulación

1. **Cola de listos:** al inicio, todos los procesos se encolan en el orden en que aparecen en el archivo.
2. **Asignación:** en cada tick, cada procesador ocioso toma el proceso al frente de la cola.
3. **Ejecución:** un tick equivale a una unidad de tiempo simulado. Un proceso con `H` hilos en un sistema con `K` hilos avanza `min(H, K)` pulsos por tick (paralelismo a nivel de hilos).
4. **Fin de turno:** cuando el quantum restante llega a 0, se descuenta una iteración.
   - Si quedan iteraciones, el proceso regresa al final de la cola.
   - Si no quedan, el proceso termina.
5. **Terminación:** la simulación finaliza cuando la cola está vacía y todos los procesadores están ociosos.

### Métricas reportadas

Por proceso: turnos completados, pulsos de CPU consumidos, ticks en CPU, tiempo total de espera en cola e instante de finalización. Globales: tiempo total simulado y utilización de CPU.

## Ejemplo de salida

Con el `config.txt` de ejemplo (fragmento):

```text
==================================================================
  SIMULADOR DE PROCESOS - Round-Robin
==================================================================
Procesadores: 2 | Hilos del sistema: 4

ID   Padre  Nombre         Regs  Tamano   Hilos  Quantum  Iter
------------------------------------------------------------------
1    0      Proceso1       5     1024     2      10       3
2    1      Proceso2       4     512      1      8        2
3    0      Proceso3       6     2048     3      12       4

--- Tick 0 | Tiempo de CPU simulado: 0 ---
  Cola (1): [Proceso3(it:4)]
  CPU0 : Proceso1     (id 1) | quantum restante: 10/10 | avance: 2 pulso(s)/tick | iter restantes: 3
  CPU1 : Proceso2     (id 2) | quantum restante: 8/8 | avance: 1 pulso(s)/tick | iter restantes: 2

--- Tick 1 | Tiempo de CPU simulado: 1 ---
  Cola (1): [Proceso3(it:4)]
  CPU0 : Proceso1     (id 1) | quantum restante: 8/10 | avance: 2 pulso(s)/tick | iter restantes: 3
  CPU1 : Proceso2     (id 2) | quantum restante: 7/8 | avance: 1 pulso(s)/tick | iter restantes: 2
...
--- Tick 5 | Tiempo de CPU simulado: 5 ---
  Cola (1): [Proceso1(it:2)]
  CPU0 : Proceso3     (id 3) | quantum restante: 12/12 | avance: 3 pulso(s)/tick | iter restantes: 4
  CPU1 : Proceso2     (id 2) | quantum restante: 3/8 | avance: 1 pulso(s)/tick | iter restantes: 2
...
```

Al final se imprime un resumen. Para el ejemplo anterior, la simulación completa dura **25 ticks** con una utilización de CPU del **94.0%**.

## Manejo de errores

El programa muestra un mensaje descriptivo (indicando el número de línea cuando aplica) y termina con código de salida distinto de 0 en los siguientes casos:

- El archivo no existe o no se puede abrir.
- Línea de configuración inválida o palabra clave desconocida.
- Falta `Procesadores` o `Hilos`, o el archivo no contiene procesos.
- Línea de proceso con un número de campos distinto de 8.
- Campos numéricos con texto, valores fuera de rango o negativos donde no corresponde.
- `ProcessId` duplicado o un proceso que es su propio padre.
- Línea de más de 511 caracteres.

Si un proceso declara un padre que no existe en el archivo, se muestra un **aviso** y la simulación continúa.

## Estructura del código

| Elemento                    | Responsabilidad                                                   |
|-----------------------------|-------------------------------------------------------------------|
| `Proceso`                   | Bloque de control de proceso (datos del archivo y métricas)       |
| `Cola` / `Nodo`             | Cola FIFO implementada con lista enlazada                         |
| `Procesador`                | Estado de cada CPU simulada                                       |
| `Sistema`                   | Configuración global y arreglo dinámico de procesos               |
| `cargar_configuracion()`    | Lectura y validación del archivo de entrada                       |
| `parsear_proceso()`         | Separación y validación de los 8 campos de cada proceso           |
| `simular()`                 | Ciclo principal de la simulación (asignar, mostrar, ejecutar)     |
| `imprimir_*()`              | Presentación del estado y del resumen final                       |
