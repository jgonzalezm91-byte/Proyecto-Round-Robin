/*
 * ============================================================================
 *  SIMULADOR DE EJECUCION DE PROCESOS (Round-Robin multiprocesador)
 * ============================================================================
 *  Descripcion:
 *    Lee un archivo de texto con la configuracion del sistema (procesadores e
 *    hilos) y la lista de procesos, y simula su planificacion con Round-Robin.
 *
 *  Uso:
 *    ./simulador [archivo_config] [retardo_ms]
 *      archivo_config : ruta del archivo de entrada (por defecto "config.txt")
 *      retardo_ms     : pausa entre ciclos en milisegundos (por defecto 300,
 *                       use 0 para ejecutar sin pausas)
 *
 *  Compilacion:
 *    Linux/macOS : gcc -std=c99 -Wall -Wextra -o simulador simulador.c
 *    Windows(GCC): gcc -std=c99 -Wall -Wextra -o simulador.exe simulador.c
 *    Windows(MSVC): cl simulador.c
 *
 *  Formato del archivo de entrada:
 *    Procesadores N
 *    Hilos K
 *    ID | IDPadre | NombrePC | Registros | Tamano | Hilos | Quantum | Iteracion
 *    (lineas vacias y lineas que empiezan con '#' se ignoran)
 *
 *  Modelo de simulacion:
 *    - Hay N procesadores; cada uno ejecuta un proceso a la vez.
 *    - Cola unica de procesos listos (FIFO) con politica Round-Robin.
 *    - Cada "tick" es una unidad de tiempo simulado.
 *    - Un proceso con H hilos, en un sistema con K hilos, avanza
 *      min(H, K) pulsos por tick (paralelismo a nivel de hilos).
 *    - Cada turno consume el Quantum del proceso; al agotarlo se descuenta
 *      una Iteracion. Si aun quedan iteraciones, el proceso vuelve al final
 *      de la cola; si no, termina.
 *    - La simulacion termina cuando la cola esta vacia y todos los
 *      procesadores estan ociosos.
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Constantes                                                                */

#define MAX_LINEA       512
#define MAX_NOMBRE      64
#define MAX_PROCESADORES 64
#define MAX_HILOS       1024
#define RETARDO_DEFECTO 300
#define ARCHIVO_DEFECTO "config.txt"


/* Estructuras de datos                                                      */


/* Bloque de control de proceso (PCB) simplificado */
typedef struct {
    int  id;                    /* ProcessId                                  */
    int  id_padre;              /* ParentProcessId (0 = proceso inicial)      */
    char nombre[MAX_NOMBRE];    /* ProgramCounter (identificador textual)     */
    int  registros;             /* Cantidad de registros                      */
    long tamano;                /* Tamano en bytes                            */
    int  hilos;                 /* Hilos del proceso                          */
    int  quantum;               /* Pulsos por turno                           */
    int  iter_total;            /* Iteraciones totales a ejecutar             */
    int  iter_restantes;        /* Iteraciones pendientes                     */
    /* Metricas */
    int  turnos;                /* Turnos completados                         */
    long cpu_pulsos;            /* Pulsos de CPU consumidos                   */
    long ticks_cpu;             /* Ticks ocupando un procesador               */
    long espera;                /* Ticks totales esperando en cola            */
    long t_encolado;            /* Instante en que entro a la cola            */
    long t_fin;                 /* Instante de finalizacion                   */
} Proceso;

/* Nodo de la cola (lista enlazada simple) */
typedef struct Nodo {
    Proceso     *proc;
    struct Nodo *sig;
} Nodo;

/* Cola FIFO de procesos listos */
typedef struct {
    Nodo *frente;
    Nodo *final;
    int   tamano;
} Cola;

/* Estado de un procesador simulado */
typedef struct {
    Proceso *proc;              /* NULL si esta ocioso                        */
    int      quantum_restante;  /* Pulsos que le quedan al turno actual       */
    int      avance;            /* Pulsos por tick (min(hilos_proc, K))       */
    long     ticks_ocupado;     /* Metrica de utilizacion                     */
} Procesador;

/* Configuracion global del sistema */
typedef struct {
    int       num_procesadores;
    int       num_hilos;
    Proceso  *procesos;
    int       num_procesos;
} Sistema;

/* Utilidades                                                                */

/* Pausa portable (milisegundos). */
static void pausa_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Elimina espacios al inicio y final (modifica la cadena). */
static char *recortar(char *s)
{
    char *fin;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    fin = s + strlen(s) - 1;
    while (fin > s && isspace((unsigned char)*fin)) *fin-- = '\0';
    return s;
}

/* Comparacion de cadenas sin distinguir mayusculas/minusculas. */
static int igual_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Convierte una cadena a long validando que sea un entero completo. */
static int parsear_long(const char *s, long *out)
{
    char *fin;
    long v;
    if (s == NULL || *s == '\0') return 0;
    errno = 0;
    v = strtol(s, &fin, 10);
    if (errno != 0 || *fin != '\0') return 0;
    *out = v;
    return 1;
}

/* Cola de procesos                                                          */

static void cola_iniciar(Cola *c)
{
    c->frente = c->final = NULL;
    c->tamano = 0;
}

static int cola_encolar(Cola *c, Proceso *p)
{
    Nodo *n = (Nodo *)malloc(sizeof(Nodo));
    if (!n) {
        fprintf(stderr, "Error: memoria insuficiente para la cola.\n");
        return 0;
    }
    n->proc = p;
    n->sig  = NULL;
    if (c->final) c->final->sig = n; else c->frente = n;
    c->final = n;
    c->tamano++;
    return 1;
}

static Proceso *cola_desencolar(Cola *c)
{
    Nodo *n;
    Proceso *p;
    if (!c->frente) return NULL;
    n = c->frente;
    p = n->proc;
    c->frente = n->sig;
    if (!c->frente) c->final = NULL;
    free(n);
    c->tamano--;
    return p;
}

static void cola_liberar(Cola *c)
{
    while (c->frente) cola_desencolar(c);
}

/* Lectura y validacion del archivo de configuracion                         */

/* Procesa una linea de proceso con 8 campos separados por '|'.
 * Devuelve 1 si es correcta; 0 si hay error (e imprime el motivo). */
static int parsear_proceso(char *linea, int nlinea, Proceso *p)
{
    char *campos[8];
    int   n = 0;
    char *ptr = linea;
    long  v[8];
    int   i;

    /* Separar por '|' */
    campos[n++] = ptr;
    while (*ptr) {
        if (*ptr == '|') {
            *ptr = '\0';
            if (n >= 8) {
                fprintf(stderr, "Error (linea %d): demasiados campos (se esperan 8).\n", nlinea);
                return 0;
            }
            campos[n++] = ptr + 1;
        }
        ptr++;
    }
    if (n != 8) {
        fprintf(stderr, "Error (linea %d): se esperan 8 campos separados por '|' y se encontraron %d.\n",
                nlinea, n);
        return 0;
    }
    for (i = 0; i < 8; i++) campos[i] = recortar(campos[i]);

    /* Campos numericos: 0,1,3,4,5,6,7 (el 2 es texto) */
    for (i = 0; i < 8; i++) {
        if (i == 2) continue;
        if (!parsear_long(campos[i], &v[i])) {
            fprintf(stderr, "Error (linea %d): el campo %d ('%s') no es un entero valido.\n",
                    nlinea, i + 1, campos[i]);
            return 0;
        }
    }
    if (campos[2][0] == '\0') {
        fprintf(stderr, "Error (linea %d): el nombre/ProgramCounter esta vacio.\n", nlinea);
        return 0;
    }
    if (v[0] <= 0)  { fprintf(stderr, "Error (linea %d): ProcessId debe ser > 0.\n", nlinea); return 0; }
    if (v[1] < 0)   { fprintf(stderr, "Error (linea %d): ParentProcessId no puede ser negativo.\n", nlinea); return 0; }
    if (v[0] == v[1]) { fprintf(stderr, "Error (linea %d): un proceso no puede ser su propio padre.\n", nlinea); return 0; }
    if (v[3] < 0)   { fprintf(stderr, "Error (linea %d): Registros no puede ser negativo.\n", nlinea); return 0; }
    if (v[4] < 0)   { fprintf(stderr, "Error (linea %d): Tamano no puede ser negativo.\n", nlinea); return 0; }
    if (v[5] <= 0)  { fprintf(stderr, "Error (linea %d): Hilos debe ser > 0.\n", nlinea); return 0; }
    if (v[6] <= 0)  { fprintf(stderr, "Error (linea %d): Quantum debe ser > 0.\n", nlinea); return 0; }
    if (v[7] <= 0)  { fprintf(stderr, "Error (linea %d): Iteracion debe ser > 0.\n", nlinea); return 0; }
    if (v[0] > 2000000000L || v[1] > 2000000000L || v[3] > 2000000000L ||
        v[5] > 2000000000L || v[6] > 2000000000L || v[7] > 2000000000L) {
        fprintf(stderr, "Error (linea %d): valor fuera de rango.\n", nlinea);
        return 0;
    }

    memset(p, 0, sizeof(*p));
    p->id             = (int)v[0];
    p->id_padre       = (int)v[1];
    strncpy(p->nombre, campos[2], MAX_NOMBRE - 1);
    p->nombre[MAX_NOMBRE - 1] = '\0';
    p->registros      = (int)v[3];
    p->tamano         = v[4];
    p->hilos          = (int)v[5];
    p->quantum        = (int)v[6];
    p->iter_total     = (int)v[7];
    p->iter_restantes = (int)v[7];
    return 1;
}

/* Lee el archivo completo. Devuelve 1 si todo fue correcto. */
static int cargar_configuracion(const char *ruta, Sistema *sis)
{
    FILE *f;
    char  linea[MAX_LINEA];
    int   nlinea = 0;
    int   tiene_proc = 0, tiene_hilos = 0;
    int   capacidad = 0;

    sis->num_procesadores = 0;
    sis->num_hilos        = 0;
    sis->procesos         = NULL;
    sis->num_procesos     = 0;

    f = fopen(ruta, "r");
    if (!f) {
        fprintf(stderr, "Error: no se pudo abrir el archivo '%s' (%s).\n", ruta, strerror(errno));
        return 0;
    }

    while (fgets(linea, sizeof(linea), f)) {
        char *s;
        nlinea++;

        /* Linea demasiado larga */
        if (strchr(linea, '\n') == NULL && !feof(f)) {
            fprintf(stderr, "Error (linea %d): la linea excede %d caracteres.\n", nlinea, MAX_LINEA - 1);
            goto fallo;
        }

        s = recortar(linea);
        /* Ignorar BOM UTF-8 si existe */
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s = recortar(s + 3);
        if (*s == '\0' || *s == '#') continue;

        if (strchr(s, '|') == NULL) {
            /* Linea de configuracion: "Palabra valor" */
            char clave[32];
            char extra[32];
            long valor;
            char valstr[32];
            int  leidos = sscanf(s, "%31s %31s %31s", clave, valstr, extra);

            if (leidos != 2 || !parsear_long(valstr, &valor)) {
                fprintf(stderr, "Error (linea %d): linea de configuracion invalida: '%s'.\n", nlinea, s);
                goto fallo;
            }
            if (igual_ci(clave, "Procesadores")) {
                if (valor < 1 || valor > MAX_PROCESADORES) {
                    fprintf(stderr, "Error (linea %d): Procesadores debe estar entre 1 y %d.\n",
                            nlinea, MAX_PROCESADORES);
                    goto fallo;
                }
                sis->num_procesadores = (int)valor;
                tiene_proc = 1;
            } else if (igual_ci(clave, "Hilos")) {
                if (valor < 1 || valor > MAX_HILOS) {
                    fprintf(stderr, "Error (linea %d): Hilos debe estar entre 1 y %d.\n", nlinea, MAX_HILOS);
                    goto fallo;
                }
                sis->num_hilos = (int)valor;
                tiene_hilos = 1;
            } else {
                fprintf(stderr, "Error (linea %d): palabra clave desconocida '%s'.\n", nlinea, clave);
                goto fallo;
            }
        } else {
            /* Linea de proceso */
            Proceso p;
            int i;
            if (!tiene_proc || !tiene_hilos) {
                fprintf(stderr, "Error (linea %d): faltan 'Procesadores' y/o 'Hilos' antes de los procesos.\n",
                        nlinea);
                goto fallo;
            }
            if (!parsear_proceso(s, nlinea, &p)) goto fallo;

            for (i = 0; i < sis->num_procesos; i++) {
                if (sis->procesos[i].id == p.id) {
                    fprintf(stderr, "Error (linea %d): ProcessId %d duplicado.\n", nlinea, p.id);
                    goto fallo;
                }
            }
            if (sis->num_procesos == capacidad) {
                int nueva = capacidad ? capacidad * 2 : 8;
                Proceso *tmp = (Proceso *)realloc(sis->procesos, (size_t)nueva * sizeof(Proceso));
                if (!tmp) {
                    fprintf(stderr, "Error: memoria insuficiente.\n");
                    goto fallo;
                }
                sis->procesos = tmp;
                capacidad = nueva;
            }
            sis->procesos[sis->num_procesos++] = p;
        }
    }

    fclose(f);

    if (!tiene_proc || !tiene_hilos) {
        fprintf(stderr, "Error: el archivo debe definir 'Procesadores' y 'Hilos'.\n");
        goto fallo_sin_cerrar;
    }
    if (sis->num_procesos == 0) {
        fprintf(stderr, "Error: el archivo no contiene ningun proceso.\n");
        goto fallo_sin_cerrar;
    }

    /* Advertencia (no fatal) si el padre no existe en el archivo */
    {
        int i, j;
        for (i = 0; i < sis->num_procesos; i++) {
            int pid = sis->procesos[i].id_padre;
            int existe = (pid == 0);
            for (j = 0; j < sis->num_procesos && !existe; j++)
                if (sis->procesos[j].id == pid) existe = 1;
            if (!existe)
                fprintf(stderr, "Aviso: el proceso %d declara como padre a %d, que no existe en el archivo.\n",
                        sis->procesos[i].id, pid);
        }
    }
    return 1;

fallo:
    fclose(f);
fallo_sin_cerrar:
    free(sis->procesos);
    sis->procesos = NULL;
    sis->num_procesos = 0;
    return 0;
}

/* Presentacion en pantalla                                                  */

static void imprimir_configuracion(const Sistema *sis)
{
    int i;
    printf("==================================================================\n");
    printf("  SIMULADOR DE PROCESOS - Round-Robin\n");
    printf("==================================================================\n");
    printf("Procesadores: %d | Hilos del sistema: %d\n\n", sis->num_procesadores, sis->num_hilos);
    printf("%-4s %-6s %-14s %-5s %-8s %-6s %-8s %-5s\n",
           "ID", "Padre", "Nombre", "Regs", "Tamano", "Hilos", "Quantum", "Iter");
    printf("------------------------------------------------------------------\n");
    for (i = 0; i < sis->num_procesos; i++) {
        const Proceso *p = &sis->procesos[i];
        printf("%-4d %-6d %-14s %-5d %-8ld %-6d %-8d %-5d\n",
               p->id, p->id_padre, p->nombre, p->registros, p->tamano,
               p->hilos, p->quantum, p->iter_total);
    }
    printf("\n");
}

static void imprimir_cola(const Cola *c)
{
    const Nodo *n;
    printf("  Cola (%d): [", c->tamano);
    for (n = c->frente; n; n = n->sig)
        printf("%s(it:%d)%s", n->proc->nombre, n->proc->iter_restantes, n->sig ? " -> " : "");
    printf("]\n");
}

static void imprimir_estado(long tiempo, const Cola *cola, const Procesador *cpus, int ncpu)
{
    int i;
    printf("--- Tick %ld | Tiempo de CPU simulado: %ld ---\n", tiempo, tiempo);
    imprimir_cola(cola);
    for (i = 0; i < ncpu; i++) {
        if (cpus[i].proc) {
            const Proceso *p = cpus[i].proc;
            printf("  CPU%-2d: %-12s (id %d) | quantum restante: %d/%d | avance: %d pulso(s)/tick | iter restantes: %d\n",
                   i, p->nombre, p->id, cpus[i].quantum_restante, p->quantum,
                   cpus[i].avance, p->iter_restantes);
        } else {
            printf("  CPU%-2d: ociosa\n", i);
        }
    }
    printf("\n");
}

static void imprimir_resumen(const Sistema *sis, const Procesador *cpus, long tiempo)
{
    int  i;
    long ocupado = 0;
    double util;

    printf("==================================================================\n");
    printf("  RESUMEN FINAL\n");
    printf("==================================================================\n");
    printf("%-4s %-12s %-7s %-8s %-9s %-8s %-8s\n",
           "ID", "Nombre", "Turnos", "Pulsos", "Ticks CPU", "Espera", "Fin");
    printf("------------------------------------------------------------------\n");
    for (i = 0; i < sis->num_procesos; i++) {
        const Proceso *p = &sis->procesos[i];
        printf("%-4d %-12s %-7d %-8ld %-9ld %-8ld %-8ld\n",
               p->id, p->nombre, p->turnos, p->cpu_pulsos, p->ticks_cpu, p->espera, p->t_fin);
    }
    for (i = 0; i < sis->num_procesadores; i++) ocupado += cpus[i].ticks_ocupado;
    util = (tiempo > 0) ? (100.0 * (double)ocupado) / ((double)tiempo * sis->num_procesadores) : 0.0;
    printf("\nTiempo total simulado : %ld ticks\n", tiempo);
    printf("Utilizacion de CPU    : %.1f%%\n", util);
    printf("Todos los procesos han terminado. Fin de la simulacion.\n");
}

/* Simulacion                                                                */

static int simular(Sistema *sis, int retardo_ms)
{
    Cola        cola;
    Procesador *cpus;
    long        tiempo = 0;
    int         i, ocupadas;

    cola_iniciar(&cola);
    cpus = (Procesador *)calloc((size_t)sis->num_procesadores, sizeof(Procesador));
    if (!cpus) {
        fprintf(stderr, "Error: memoria insuficiente.\n");
        return 0;
    }

    /* Cola inicial: orden del archivo */
    for (i = 0; i < sis->num_procesos; i++) {
        sis->procesos[i].t_encolado = 0;
        if (!cola_encolar(&cola, &sis->procesos[i])) {
            cola_liberar(&cola);
            free(cpus);
            return 0;
        }
    }

    do {
        /*  Asignar procesos de la cola a los procesadores ociosos */
        for (i = 0; i < sis->num_procesadores; i++) {
            if (cpus[i].proc == NULL && cola.tamano > 0) {
                Proceso *p = cola_desencolar(&cola);
                p->espera += tiempo - p->t_encolado;
                cpus[i].proc             = p;
                cpus[i].quantum_restante = p->quantum;
                cpus[i].avance           = (p->hilos < sis->num_hilos) ? p->hilos : sis->num_hilos;
            }
        }

        /*  Mostrar estado actual */
        imprimir_estado(tiempo, &cola, cpus, sis->num_procesadores);
        pausa_ms(retardo_ms);

        /*  Ejecutar un tick en cada procesador ocupado */
        tiempo++;
        for (i = 0; i < sis->num_procesadores; i++) {
            Proceso *p = cpus[i].proc;
            int consumo;
            if (!p) continue;

            consumo = (cpus[i].avance < cpus[i].quantum_restante) ? cpus[i].avance
                                                                  : cpus[i].quantum_restante;
            cpus[i].quantum_restante -= consumo;
            cpus[i].ticks_ocupado++;
            p->cpu_pulsos += consumo;
            p->ticks_cpu++;

            /*  Quantum agotado: fin de turno */
            if (cpus[i].quantum_restante == 0) {
                p->turnos++;
                p->iter_restantes--;
                cpus[i].proc = NULL;
                if (p->iter_restantes > 0) {
                    p->t_encolado = tiempo;
                    if (!cola_encolar(&cola, p)) {
                        cola_liberar(&cola);
                        free(cpus);
                        return 0;
                    }
                } else {
                    p->t_fin = tiempo;
                }
            }
        }

        /*  Continuar mientras haya cola o procesadores ocupados */
        ocupadas = 0;
        for (i = 0; i < sis->num_procesadores; i++)
            if (cpus[i].proc) ocupadas++;

    } while (cola.tamano > 0 || ocupadas > 0);

    /* Estado final */
    imprimir_estado(tiempo, &cola, cpus, sis->num_procesadores);
    imprimir_resumen(sis, cpus, tiempo);

    cola_liberar(&cola);
    free(cpus);
    return 1;
}

/* Programa principal                                                        */

int main(int argc, char *argv[])
{
    const char *ruta   = ARCHIVO_DEFECTO;
    int         retardo = RETARDO_DEFECTO;
    Sistema     sis;
    int         ok;

    if (argc > 1) ruta = argv[1];
    if (argc > 2) {
        long v;
        if (!parsear_long(argv[2], &v) || v < 0 || v > 60000) {
            fprintf(stderr, "Error: el retardo debe ser un entero entre 0 y 60000 (ms).\n");
            return EXIT_FAILURE;
        }
        retardo = (int)v;
    }
    if (argc > 3) {
        fprintf(stderr, "Uso: %s [archivo_config] [retardo_ms]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (!cargar_configuracion(ruta, &sis)) return EXIT_FAILURE;

    imprimir_configuracion(&sis);
    ok = simular(&sis, retardo);

    free(sis.procesos);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
