// parking2.cpp : Este archivo contiene la función "main". La ejecución del programa comienza y termina ahí.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>
#include "parking2.h"

#define MAX_HILOS 1000
#define PASOS_SALIR_ACERA 2 

typedef int (*TIPO_PARKING2_INICIO)(TIPO_FUNCION_LLEGADA*, TIPO_FUNCION_SALIDA*, long, int);
typedef int (*TIPO_PARKING2_FIN)(void);
typedef int (*TIPO_PARKING2_APARCAR)(HCoche, void*, TIPO_FUNCION_APARCAR_COMMIT, TIPO_FUNCION_PERMISO_AVANCE, TIPO_FUNCION_PERMISO_AVANCE_COMMIT);
typedef int (*TIPO_PARKING2_DESAPARCAR)(HCoche, void*, TIPO_FUNCION_PERMISO_AVANCE, TIPO_FUNCION_PERMISO_AVANCE_COMMIT);
typedef int (*TIPO_PARKING2_getNUmero)(HCoche);
typedef int (*TIPO_PARKING2_getLongitud)(HCoche);
typedef void* (*TIPO_PARKING2_getDatos)(HCoche);
typedef int (*TIPO_PARKING2_getX)(HCoche);
typedef int (*TIPO_PARKING2_getY)(HCoche);
typedef int (*TIPO_PARKING2_getX2)(HCoche);
typedef int (*TIPO_PARKING2_getY2)(HCoche);
typedef int (*TIPO_PARKING2_getAlgoritmo)(HCoche);

TIPO_PARKING2_INICIO PARKING2_inicio = NULL;
TIPO_PARKING2_FIN PARKING2_fin = NULL;
TIPO_PARKING2_APARCAR PARKING2_aparcar = NULL; 
TIPO_PARKING2_DESAPARCAR PARKING2_desaparcar = NULL;
TIPO_PARKING2_getNUmero PARKING2_getNUmero = NULL;
TIPO_PARKING2_getLongitud PARKING2_getLongitud = NULL;
TIPO_PARKING2_getDatos PARKING2_getDatos = NULL;
TIPO_PARKING2_getX PARKING2_getX = NULL;
TIPO_PARKING2_getY PARKING2_getY = NULL;
TIPO_PARKING2_getX2 PARKING2_getX2 = NULL;
TIPO_PARKING2_getY2 PARKING2_getY2 = NULL;
TIPO_PARKING2_getAlgoritmo PARKING2_getAlgoritmo = NULL;

FILE* fichero_debug = NULL;

typedef struct {
    int aceras[4][80];
    int ocupante[4][3][80]; 
    int hilo_esperando[MAX_HILOS]; 
    int hilo_libre[MAX_HILOS]; 
    int ultimo_aparcado_sa;
    int siguiente_coche_aparcar[4]; 
}MEMORIA_PARKING;

typedef struct {
    HCoche hc; 
    int id_hilo; 
    int desaparcando;
    int pasos_verticales_desaparcando;
} DATOS_HILO;

HANDLE eventoCtrlC = NULL;

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    if (fdwCtrlType == CTRL_C_EVENT)
    {
        if (eventoCtrlC != NULL)
            SetEvent(eventoCtrlC);

        return TRUE;
    }
    return FALSE;
}

int abrir_recursos_memoria(HANDLE* mutex_memoria, HANDLE* zona_memoria, MEMORIA_PARKING** memoria_parking) {
    *mutex_memoria = NULL;
    *zona_memoria = NULL;
    *memoria_parking = NULL;
    *mutex_memoria = OpenMutex(MUTEX_ALL_ACCESS, FALSE, "MUTEX_MEMORIA_PARKING2");
    if (*mutex_memoria == NULL) {
        PERROR("OpenMutex");
        return -1;
    }

    *zona_memoria = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, "MEMORIA_PARKING2");
    if (*zona_memoria == NULL) {
        PERROR("OpenFileMapping");
        CloseHandle(*mutex_memoria);
        *mutex_memoria = NULL;
        return -1;
    }

    *memoria_parking = (MEMORIA_PARKING*)MapViewOfFile(*zona_memoria, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MEMORIA_PARKING));
    if (*memoria_parking == NULL) {
        PERROR("MapViewOfFile");
        CloseHandle(*zona_memoria); 
        CloseHandle(*mutex_memoria);
        *zona_memoria = NULL;
        *mutex_memoria = NULL;
        return -1;
    }
    return 0;
}

void cerrar_recursos_memoria(HANDLE mutex_memoria, HANDLE zona_memoria, MEMORIA_PARKING* memoria_parking)
{
    if (memoria_parking != NULL)
        UnmapViewOfFile(memoria_parking);

    if (zona_memoria != NULL)
        CloseHandle(zona_memoria);

    if (mutex_memoria != NULL)
        CloseHandle(mutex_memoria);
}

void avisar_cambios(MEMORIA_PARKING* memoria_parking) 
{
    HANDLE semaforo_hilo;
    char nombre_semaforo[32];

    for (int k = 0; k < MAX_HILOS; k++) {
        if (memoria_parking->hilo_esperando[k] == 1) {
            memoria_parking->hilo_esperando[k] = 0;

            sprintf_s(nombre_semaforo, sizeof(nombre_semaforo), "SEM_hilo_%d", k); 
            semaforo_hilo = OpenSemaphore(SEMAPHORE_ALL_ACCESS, FALSE, nombre_semaforo); 
            if (semaforo_hilo != NULL) {
                ReleaseSemaphore(semaforo_hilo, 1, NULL); 
                CloseHandle(semaforo_hilo);
            }
        }
    }
}

int reservar_destino(HCoche hc, int x_dest, int y_dest, MEMORIA_PARKING* memoria_parking)
{
    int num = PARKING2_getNUmero(hc);
    int longitud = PARKING2_getLongitud(hc);
    int alg = PARKING2_getAlgoritmo(hc);

    for (int i = 0; i < longitud; i++) {
        int x = x_dest + i; 
        if (x < 0 || x >= 80) 
            continue;

        if (memoria_parking->ocupante[alg][y_dest][x] != 0 &&
            memoria_parking->ocupante[alg][y_dest][x] != num) {
            return 0;
        }
    }

    for (int i = 0; i < longitud; i++) {
        int x = x_dest + i;
        if (x < 0 || x >= 80)
            continue;
        memoria_parking->ocupante[alg][y_dest][x] = num;
    }

    return 1;
}

void liberar_origen_tras_commit(HCoche hc, int x_old, int y_old, MEMORIA_PARKING* memoria_parking)
{
    int num = PARKING2_getNUmero(hc);
    int longitud = PARKING2_getLongitud(hc);
    int x_new = PARKING2_getX(hc); 
    int y_new = PARKING2_getY(hc);
    int alg = PARKING2_getAlgoritmo(hc);

    for (int i = 0; i < longitud; i++) { 
        int x = x_old + i;
        if (x < 0 || x >= 80)
            continue;

        if (!(y_old == y_new && x >= x_new && x < x_new + longitud)) {
            if (memoria_parking->ocupante[alg][y_old][x] == num) {
                memoria_parking->ocupante[alg][y_old][x] = 0;
            }
        }
    }
}

void callback_aparcar_commit(HCoche hc)
{
    HANDLE mutex_memoria;
    HANDLE zona_memoria;
    MEMORIA_PARKING* memoria_parking;
    int alg = PARKING2_getAlgoritmo(hc);

    if (abrir_recursos_memoria(&mutex_memoria, &zona_memoria, &memoria_parking) == -1)
        return;

    DWORD esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
    if (esperar_mutex == WAIT_OBJECT_0)
    {
        memoria_parking->siguiente_coche_aparcar[alg]++;
        avisar_cambios(memoria_parking);

        ReleaseMutex(mutex_memoria);
    }

    cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
}

void callback_permiso_avance(HCoche hc)
{
    HANDLE mutex_memoria;
    HANDLE zona_memoria;
    HANDLE semaforo_hilo;
    MEMORIA_PARKING* memoria_parking;
    char nombre_semaforo[32];

    if (abrir_recursos_memoria(&mutex_memoria, &zona_memoria, &memoria_parking) == -1)
        return;

    DATOS_HILO* datos = (DATOS_HILO*)PARKING2_getDatos(hc);
    if (datos == NULL) {
        fprintf(stderr, "Error: datos del coche no validos\n");
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return;
    }
    
    int id_hilo = datos->id_hilo;
    if (id_hilo >= MAX_HILOS) {
        fprintf(stderr, "Error: id_hilo no valido\n");
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return;
    }

    sprintf_s(nombre_semaforo, sizeof(nombre_semaforo), "SEM_hilo_%d", id_hilo);

    semaforo_hilo = OpenSemaphore(SEMAPHORE_ALL_ACCESS, FALSE, nombre_semaforo);
    if (semaforo_hilo == NULL) {
        PERROR("OpenSemaphore");
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return;
    }

    while (1)
    {
        int x_dest = PARKING2_getX2(hc);
        int y_dest = PARKING2_getY2(hc);

        DWORD esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
        if (esperar_mutex != WAIT_OBJECT_0){
            CloseHandle(semaforo_hilo);
            cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
            return;
        }

        if (reservar_destino(hc, x_dest, y_dest, memoria_parking) == 1){
            ReleaseMutex(mutex_memoria);
            break;
        }

        memoria_parking->hilo_esperando[id_hilo] = 1;
        ReleaseMutex(mutex_memoria);
        
        DWORD esperar_semaforo = WaitForSingleObject(semaforo_hilo, INFINITE);
        if (esperar_semaforo != WAIT_OBJECT_0) {
            CloseHandle(semaforo_hilo);
            cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
            return;
        }

    }

    CloseHandle(semaforo_hilo);
    cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
}

void callback_permiso_avance_commit(HCoche hc)
{
    HANDLE mutex_memoria;
    HANDLE zona_memoria;
    MEMORIA_PARKING* memoria_parking;

    int x_old = PARKING2_getX2(hc);
    int y_old = PARKING2_getY2(hc);
    int x_new = PARKING2_getX(hc);
    int y_new = PARKING2_getY(hc);
    int num = PARKING2_getNUmero(hc);
    int alg = PARKING2_getAlgoritmo(hc);
    DATOS_HILO* datos = (DATOS_HILO*)PARKING2_getDatos(hc);

    if (abrir_recursos_memoria(&mutex_memoria, &zona_memoria, &memoria_parking) == -1)
        return;

    DWORD esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
    if (esperar_mutex != WAIT_OBJECT_0)
    {
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return;
    }

    liberar_origen_tras_commit(hc, x_old, y_old, memoria_parking);

    reservar_destino(hc, x_new, y_new, memoria_parking);

    if (datos != NULL && datos->desaparcando)
    {
        if (x_old == x_new && y_old != y_new)
        {
            datos->pasos_verticales_desaparcando++;
        }

        if (datos->pasos_verticales_desaparcando == PASOS_SALIR_ACERA)
        {
            for (int j = 0; j < 80; j++)
            {
                if (memoria_parking->aceras[alg][j] == num)
                    memoria_parking->aceras[alg][j] = 0;
            }

            datos->desaparcando = 0;
            datos->pasos_verticales_desaparcando = 0;
        }
    }
    avisar_cambios(memoria_parking);

    ReleaseMutex(mutex_memoria);
    cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
}

void liberar_hilo_coche(int id_hilo) 
{
    HANDLE mutex_memoria;
    HANDLE zona_memoria;
    MEMORIA_PARKING* memoria_parking;

    if (abrir_recursos_memoria(&mutex_memoria, &zona_memoria, &memoria_parking) == -1)
        return;

    DWORD esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
    if (esperar_mutex != WAIT_OBJECT_0)
    {
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return;
    }

    if (id_hilo < MAX_HILOS)
    {
        memoria_parking->hilo_libre[id_hilo] = 1;
        memoria_parking->hilo_esperando[id_hilo] = 0;
    }

    ReleaseMutex(mutex_memoria);
    cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
}

DWORD WINAPI hilo_aparcar(LPVOID parametro)
{
    DATOS_HILO* datos = (DATOS_HILO*) parametro;

    HCoche hc = datos->hc;
    int id_hilo = datos->id_hilo;

    int num = PARKING2_getNUmero(hc);
    int alg = PARKING2_getAlgoritmo(hc);

    HANDLE mutex_memoria;
    HANDLE zona_memoria;
    MEMORIA_PARKING* memoria_parking;

    HANDLE semaforo_hilo;
    char nombre_semaforo[32];

    sprintf_s(nombre_semaforo, sizeof(nombre_semaforo), "SEM_hilo_%d", id_hilo);
    semaforo_hilo = OpenSemaphore(SEMAPHORE_ALL_ACCESS, FALSE, nombre_semaforo);

    if (semaforo_hilo == NULL) {
        PERROR("OpenSemaphore");
        liberar_hilo_coche(id_hilo);
        free(datos);
        return (DWORD)-1;
    }

    if (abrir_recursos_memoria(&mutex_memoria, &zona_memoria, &memoria_parking) == -1) {
        CloseHandle(semaforo_hilo);
        liberar_hilo_coche(id_hilo);
        free(datos);
        return (DWORD)-1;
    }

    DWORD esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
    if (esperar_mutex != WAIT_OBJECT_0) {
        fprintf(stderr, "Error al esperar el mutex en hilo_aparcar\n");
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        CloseHandle(semaforo_hilo);
        liberar_hilo_coche(id_hilo);
        free(datos);
        return (DWORD)-1;
    }

    while (num != memoria_parking->siguiente_coche_aparcar[alg]) {
        memoria_parking->hilo_esperando[id_hilo] = 1;

        ReleaseMutex(mutex_memoria);

        DWORD esperar_semaforo = WaitForSingleObject(semaforo_hilo, INFINITE);
        if (esperar_semaforo != WAIT_OBJECT_0) {
            fprintf(stderr, "Error al esperar el semaforo en hilo_aparcar\n");
            cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
            CloseHandle(semaforo_hilo);
            liberar_hilo_coche(id_hilo);
            free(datos);
            return (DWORD)-1;
        }

        esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
        if (esperar_mutex != WAIT_OBJECT_0) {
            fprintf(stderr, "Error al recuperar el mutex en hilo_aparcar\n");
            cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
            CloseHandle(semaforo_hilo);
            liberar_hilo_coche(id_hilo);
            free(datos);
            return (DWORD)-1;
        }
    }

    memoria_parking->hilo_esperando[id_hilo] = 0;

    ReleaseMutex(mutex_memoria);
    cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
    CloseHandle(semaforo_hilo);

    if (PARKING2_aparcar(hc, datos, callback_aparcar_commit, callback_permiso_avance, callback_permiso_avance_commit) == -1) {
        PERROR("PARKING2_aparcar");
        liberar_hilo_coche(id_hilo);
        free(datos);
        return (DWORD)-1;
    }

    liberar_hilo_coche(id_hilo);
    return 0;
}

DWORD WINAPI hilo_desaparcar(LPVOID parametro)
{
    DATOS_HILO* datos = (DATOS_HILO*)parametro;

    HCoche hc = datos->hc;
    int id_hilo = datos->id_hilo;

    if (PARKING2_desaparcar(hc, datos, callback_permiso_avance, callback_permiso_avance_commit) == -1) {
        PERROR("PARKING2_desaparcar");
        liberar_hilo_coche(id_hilo);
        free(datos);
        return (DWORD)-1;
    }

    liberar_hilo_coche(id_hilo);
    free(datos);
    return 0;
}

int obtener_hilo_libre(MEMORIA_PARKING* memoria_parking)
{
    for (int i = 0; i < MAX_HILOS; i++) {
        if (memoria_parking->hilo_libre[i] == 1) {
            memoria_parking->hilo_libre[i] = 0; 
            return i; 
        }
    }
    return -1;
}

int crear_hilo_aparcar(HCoche hc, int id_hilo)
{
    HANDLE hiloAparcar; 
    DATOS_HILO* datoshilo = (DATOS_HILO*)malloc(sizeof(DATOS_HILO));

    if (datoshilo == NULL) {
        fprintf(stderr, "Error reservando memoria para datoshilo\n");
        return -1;
    }

    datoshilo->hc = hc;
    datoshilo->id_hilo = id_hilo;
    datoshilo->desaparcando = 0;
    datoshilo->pasos_verticales_desaparcando = 0;

    hiloAparcar = CreateThread(NULL, 0, hilo_aparcar, datoshilo, 0, NULL); 
    if (hiloAparcar == NULL) {
        PERROR("CreateThread");
        free(datoshilo);
        return -1;
    }
    CloseHandle(hiloAparcar);
    
    return 0;
}

 int intentar_aparcar(HCoche hc, int alg, int pos, MEMORIA_PARKING* memoria_parking)
{
    int num = PARKING2_getNUmero(hc);
    int longitud = PARKING2_getLongitud(hc);

    if (pos < 0 || pos + longitud > 80) 
        return -1;

    for (int j = 0; j < longitud; j++) {
        memoria_parking->aceras[alg][pos + j] = num;
    }

    int id_hilo = obtener_hilo_libre(memoria_parking);
    if (id_hilo == -1) {

        for (int j = 0; j < longitud; j++) {
            memoria_parking->aceras[alg][pos + j] = 0;
        }
        return -1;
    }

    if (crear_hilo_aparcar(hc, id_hilo) == -1) {
        memoria_parking->hilo_libre[id_hilo] = 1;

        for (int j = 0; j < longitud; j++) {
            memoria_parking->aceras[alg][pos + j] = 0;
        }

        return -1;
    }
    return 0;
}

 int crear_hilo_desaparcar(DATOS_HILO* datoshilo)
 {
     HANDLE hiloDesaparcar;

     hiloDesaparcar = CreateThread(NULL, 0, hilo_desaparcar, datoshilo, 0, NULL);
     if (hiloDesaparcar == NULL) {
         PERROR("CreateThread hilo_desaparcar");
         return -1;
     }

     CloseHandle(hiloDesaparcar);
     return 0;
 }

int llegada_primer_ajuste(HCoche hc)
{
    HANDLE mutex_memoria;
    HANDLE zona_memoria;
    MEMORIA_PARKING* memoria_parking;

    if (abrir_recursos_memoria(&mutex_memoria, &zona_memoria, &memoria_parking) == -1)
        return -1;

    int num = PARKING2_getNUmero(hc);
    int longitud = PARKING2_getLongitud(hc);

    DWORD esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
    if (esperar_mutex != WAIT_OBJECT_0) {
        fprintf(stderr, "Error al esperar el mutex en llegada_primer_ajuste\n");
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return -1;
    }

    for (int pos = 0; pos <= 80 - longitud; pos++) 
    {
        int cabe = 1;
        for (int j = 0; j < longitud; j++) 
        {
            if (memoria_parking->aceras[PRIMER_AJUSTE][pos + j] != 0 &&
                memoria_parking->aceras[PRIMER_AJUSTE][pos + j] != num) {
                cabe = 0;
                break;
            }
        }
        if (cabe) {
            if (intentar_aparcar(hc, PRIMER_AJUSTE, pos, memoria_parking) == 0) {
                ReleaseMutex(mutex_memoria);
                cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
                return pos;
            }
        }
    }

    ReleaseMutex(mutex_memoria);
    cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
    return -1;
}

int llegada_siguiente_ajuste(HCoche hc)
{
    HANDLE mutex_memoria;
    HANDLE zona_memoria;
    MEMORIA_PARKING* memoria_parking;

    if (abrir_recursos_memoria(&mutex_memoria, &zona_memoria, &memoria_parking) == -1)
        return -1;

    int num = PARKING2_getNUmero(hc);
    int longitud = PARKING2_getLongitud(hc);

    DWORD esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
    if (esperar_mutex != WAIT_OBJECT_0) {
        fprintf(stderr, "Error al esperar el mutex en llegada_siguiente_ajuste\n");
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return -1;
    }

    int pos_inicio = memoria_parking->ultimo_aparcado_sa;

    if (memoria_parking->aceras[SIGUIENTE_AJUSTE][pos_inicio] == 0) {
        while (pos_inicio > 0 && memoria_parking->aceras[SIGUIENTE_AJUSTE][pos_inicio - 1] == 0) {
            pos_inicio--;
        }
    }

    for (int intento = 0; intento < 80; intento++) 
    {
        int pos_actual = (pos_inicio + intento) % 80;

        if (pos_actual + longitud > 80)
            continue;

        int cabe = 1;
        for (int j = 0; j < longitud; j++) {
            if (memoria_parking->aceras[SIGUIENTE_AJUSTE][pos_actual + j] != 0 &&
                memoria_parking->aceras[SIGUIENTE_AJUSTE][pos_actual + j] != num) {
                cabe = 0;
                break;
            }
        }
        if (cabe) {
            if (intentar_aparcar(hc, SIGUIENTE_AJUSTE, pos_actual, memoria_parking) == 0) {
                memoria_parking->ultimo_aparcado_sa = pos_actual;
                ReleaseMutex(mutex_memoria);
                cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
                return pos_actual;
            }
        }
    }

    ReleaseMutex(mutex_memoria);
    cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
    return -1;
}

int llegada_mejor_ajuste(HCoche hc)
{
    HANDLE mutex_memoria;
    HANDLE zona_memoria;
    MEMORIA_PARKING* memoria_parking;

    if (abrir_recursos_memoria(&mutex_memoria, &zona_memoria, &memoria_parking) == -1)
        return -1;

    int num = PARKING2_getNUmero(hc);
    int longitud = PARKING2_getLongitud(hc);

    DWORD esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
    if (esperar_mutex != WAIT_OBJECT_0) {
        fprintf(stderr, "Error al esperar el mutex en  llegada_mejor_ajuste\n");
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return -1;
    }

    int pos = 0;
    int mejor_tam = 81; 
    int mejor_pos = -1;

    while (pos < 80) {
        if (memoria_parking->aceras[MEJOR_AJUSTE][pos] == 0 ||
            memoria_parking->aceras[MEJOR_AJUSTE][pos] == num) {
            int inicio_hueco = pos;
            int tam_hueco = 0;
            while (pos < 80 && (memoria_parking->aceras[MEJOR_AJUSTE][pos] == 0 ||
                memoria_parking->aceras[MEJOR_AJUSTE][pos] == num)) {
                tam_hueco++;
                pos++;
            }
            if (tam_hueco >= longitud && tam_hueco < mejor_tam) {
                mejor_tam = tam_hueco;
                mejor_pos = inicio_hueco;
            }
        }
        else {
            pos++;
        }
    }
    if (mejor_pos != -1) {
        if (intentar_aparcar(hc, MEJOR_AJUSTE, mejor_pos, memoria_parking) == 0) {
            ReleaseMutex(mutex_memoria);
            cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
            return mejor_pos;
        }
    }
    ReleaseMutex(mutex_memoria);
    cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
    return -1;

}

int llegada_peor_ajuste(HCoche hc)
{
    HANDLE mutex_memoria;
    HANDLE zona_memoria;
    MEMORIA_PARKING* memoria_parking;

    if (abrir_recursos_memoria(&mutex_memoria, &zona_memoria, &memoria_parking) == -1)
        return -1;

    int num = PARKING2_getNUmero(hc);
    int longitud = PARKING2_getLongitud(hc);

    DWORD esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
    if (esperar_mutex != WAIT_OBJECT_0) {
        fprintf(stderr, "Error al esperar el mutex en llegada_peor_ajuste\n");
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return -1;
    }

    int pos = 0;
    int peor_tam = -1;
    int peor_pos = -1;
    while (pos < 80) {
        if (memoria_parking->aceras[PEOR_AJUSTE][pos] == 0 ||
            memoria_parking->aceras[PEOR_AJUSTE][pos] == num) {
            int inicio_hueco = pos;
            int tam_hueco = 0;
            while (pos < 80 && (memoria_parking->aceras[PEOR_AJUSTE][pos] == 0 ||
                memoria_parking->aceras[PEOR_AJUSTE][pos] == num)) {
                tam_hueco++;
                pos++;
            }
            if (tam_hueco >= longitud && tam_hueco > peor_tam) {
                peor_tam = tam_hueco;
                peor_pos = inicio_hueco;
            }
        }
        else {
            pos++;
        }
    }
    if (peor_pos != -1) {
        if (intentar_aparcar(hc, PEOR_AJUSTE, peor_pos, memoria_parking) == 0) {
            ReleaseMutex(mutex_memoria);
            cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
            return peor_pos;
        }
    }

    ReleaseMutex(mutex_memoria);
    cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
    return -1;
}

int funcion_salida(HCoche hc)
{
    HANDLE mutex_memoria;
    HANDLE zona_memoria;
    MEMORIA_PARKING* memoria_parking;

    if (abrir_recursos_memoria(&mutex_memoria, &zona_memoria, &memoria_parking) == -1)
        return -1;

    DATOS_HILO* datos = (DATOS_HILO*)PARKING2_getDatos(hc);
    if (datos == NULL) {
        fprintf(stderr, "Error: datos apunta a NULL en funcion_salida\n");
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return -1;
    }

    DWORD esperar_mutex = WaitForSingleObject(mutex_memoria, INFINITE);
    if (esperar_mutex != WAIT_OBJECT_0) {
        fprintf(stderr, "Error al esperar el mutex en funcion_salida\n");
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return -1;
    }

    int id_hilo = obtener_hilo_libre(memoria_parking);
    if (id_hilo == -1) {
        fprintf(stderr, "Error: no hay hiloes libres para desaparcar\n");

        ReleaseMutex(mutex_memoria);
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return -1;
    }

    datos->id_hilo = id_hilo;
    datos->desaparcando = 1;
    datos->pasos_verticales_desaparcando = 0;

    if (crear_hilo_desaparcar(datos) == -1) {
        memoria_parking->hilo_libre[id_hilo] = 1;

        datos->desaparcando = 0;
        datos->pasos_verticales_desaparcando = 0;

        ReleaseMutex(mutex_memoria);
        cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
        return -1;
    }

    ReleaseMutex(mutex_memoria);
    cerrar_recursos_memoria(mutex_memoria, zona_memoria, memoria_parking);
    return 0;
}


int main(int argc, char* argv[])
{
    BOOL debug = FALSE;

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Uso: %s <retardo> [D]\n", argv[0]);
        return -1;
    }

    char* fin;
    long retardo = strtol(argv[1], &fin, 10); 
    if (*argv[1] == '\0' || *fin != '\0' || retardo < 0) { 
        fprintf(stderr, "Error: la velocidad debe de ser un entero mayor o igual que 0\n");
        return -1;
    }

    if (argc == 3) {
        if (strcmp(argv[2], "D") != 0) {
            fprintf(stderr, "Error: el segundo argumento solo puede ser D\n");
            return -1;
        }

        debug = TRUE;

        if (freopen_s(&fichero_debug, "debug.log", "w", stderr) != 0 || fichero_debug == NULL) {
            fprintf(stderr, "Error al abrir el fichero de debug\n");
            return -1;
        }
    }

    eventoCtrlC = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (eventoCtrlC == NULL)
    {
        PERROR("CreateEvent");
        return -1;
    }

    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE))
    {
        PERROR("SetConsoleCtrlHandler");
        CloseHandle(eventoCtrlC);
        eventoCtrlC = NULL;
        return -1;
    }

    HINSTANCE libreria = LoadLibrary("parking.dll");
    if (libreria == NULL) {
        PERROR("LoadLibrary");
        return -1;
    }

    PARKING2_inicio = (TIPO_PARKING2_INICIO)GetProcAddress(libreria, "PARKING2_inicio");
    if (PARKING2_inicio == NULL) {
        PERROR("GetProcAddress PARKING2_inicio");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_fin = (TIPO_PARKING2_FIN)GetProcAddress(libreria, "PARKING2_fin");
    if (PARKING2_fin == NULL) {
        PERROR("GetProcAddress PARKING2_fin");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_aparcar = (TIPO_PARKING2_APARCAR)GetProcAddress(libreria, "PARKING2_aparcar");
    if (PARKING2_aparcar == NULL) {
        PERROR("GetProcAddress PARKING2_aparcar");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_desaparcar = (TIPO_PARKING2_DESAPARCAR)GetProcAddress(libreria, "PARKING2_desaparcar");
    if (PARKING2_desaparcar == NULL) {
        PERROR("GetProcAddress PARKING2_desaparcar");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_getNUmero = (TIPO_PARKING2_getNUmero)GetProcAddress(libreria, "PARKING2_getNUmero");
    if (PARKING2_getNUmero == NULL) {
        PERROR("GetProcAddress PARKING2_getNUmero");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_getLongitud = (TIPO_PARKING2_getLongitud)GetProcAddress(libreria, "PARKING2_getLongitud");
    if (PARKING2_getLongitud == NULL) {
        PERROR("GetProcAddress PARKING2_getLongitud");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_getDatos = (TIPO_PARKING2_getDatos)GetProcAddress(libreria, "PARKING2_getDatos");
    if (PARKING2_getDatos == NULL) {
        PERROR("GetProcAddress PARKING2_getDatos");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_getX = (TIPO_PARKING2_getX)GetProcAddress(libreria, "PARKING2_getX");
    if (PARKING2_getX == NULL) {
        PERROR("GetProcAddress PARKING2_getX");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_getY = (TIPO_PARKING2_getY)GetProcAddress(libreria, "PARKING2_getY");
    if (PARKING2_getY == NULL) {
        PERROR("GetProcAddress PARKING2_getY");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_getX2 = (TIPO_PARKING2_getX2)GetProcAddress(libreria, "PARKING2_getX2");
    if (PARKING2_getX2 == NULL) {
        PERROR("GetProcAddress PARKING2_getX2");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_getY2 = (TIPO_PARKING2_getY2)GetProcAddress(libreria, "PARKING2_getY2");
    if (PARKING2_getY2 == NULL) {
        PERROR("GetProcAddress PARKING2_getY2");
        FreeLibrary(libreria);
        return -1;
    }

    PARKING2_getAlgoritmo = (TIPO_PARKING2_getAlgoritmo)GetProcAddress(libreria, "PARKING2_getAlgoritmo");
    if (PARKING2_getAlgoritmo == NULL) {
        PERROR("GetProcAddress PARKING2_getAlgoritmo");
        FreeLibrary(libreria);
        return -1;
    }

    HANDLE mutex_memoria = CreateMutex(NULL, FALSE, "MUTEX_MEMORIA_PARKING2");
    if (mutex_memoria == NULL) {
        PERROR("CreateMutex");
        FreeLibrary(libreria);
        return -1;
    }

    HANDLE zona_memoria = CreateFileMapping((HANDLE)-1, NULL, PAGE_READWRITE, 0, sizeof(MEMORIA_PARKING), "MEMORIA_PARKING2"); 
    if (zona_memoria == NULL) {
        PERROR("CreateFileMapping");
        CloseHandle(mutex_memoria);
        FreeLibrary(libreria);
        return -1;
    }

    MEMORIA_PARKING* memoria_parking = (MEMORIA_PARKING*)MapViewOfFile(zona_memoria, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MEMORIA_PARKING));
    if (memoria_parking == NULL) {
        PERROR("MapViewOfFile");
        CloseHandle(zona_memoria);
        CloseHandle(mutex_memoria);
        FreeLibrary(libreria);
        return -1;
    }

    memset(memoria_parking, 0, sizeof(MEMORIA_PARKING));

    for (int i = 0; i < MAX_HILOS; i++) {
        memoria_parking->hilo_libre[i] = 1; 
    }

    for (int i = 0; i < 4; i++) {
        memoria_parking->siguiente_coche_aparcar[i] = 1;
    }

    HANDLE vector_sem_hilo[MAX_HILOS];

    memset(vector_sem_hilo, 0, sizeof(vector_sem_hilo));

    for (int i = 0; i < MAX_HILOS; i++) {
        char nombre_semaforo[32];
        sprintf_s(nombre_semaforo, sizeof(nombre_semaforo), "SEM_hilo_%d", i);

        vector_sem_hilo[i] = CreateSemaphore(NULL, 0, 1, nombre_semaforo);
        if (vector_sem_hilo[i] == NULL) {
            PERROR("CreateSemaphore");

            for (int j = 0; j < i; j++)
                CloseHandle(vector_sem_hilo[j]);

            UnmapViewOfFile(memoria_parking);
            CloseHandle(zona_memoria);
            CloseHandle(mutex_memoria);
            FreeLibrary(libreria);
            return -1;
        }
    }

    memoria_parking->ultimo_aparcado_sa = 0;

    TIPO_FUNCION_LLEGADA f_llegadas[4];
    f_llegadas[PRIMER_AJUSTE] = llegada_primer_ajuste;
    f_llegadas[SIGUIENTE_AJUSTE] = llegada_siguiente_ajuste;
    f_llegadas[MEJOR_AJUSTE] = llegada_mejor_ajuste;
    f_llegadas[PEOR_AJUSTE] = llegada_peor_ajuste;

    TIPO_FUNCION_SALIDA f_salidas[4];
    for (int i = 0; i < 4; i++)
        f_salidas[i] = funcion_salida;

    if (PARKING2_inicio(f_llegadas, f_salidas, retardo, debug) == -1) {
        PERROR("PARKING2_inicio");
        for (int i = 0; i < MAX_HILOS; i++) {
            if (vector_sem_hilo[i] != NULL)
                CloseHandle(vector_sem_hilo[i]);
        }
        UnmapViewOfFile(memoria_parking);
        CloseHandle(zona_memoria);
        CloseHandle(mutex_memoria);
        if (eventoCtrlC != NULL) {
            CloseHandle(eventoCtrlC);
            eventoCtrlC = NULL;
        }
        FreeLibrary(libreria); 
        return -1;
    }

    DWORD control_C = WaitForSingleObject(eventoCtrlC, 30000);
    if (control_C != WAIT_OBJECT_0 && control_C != WAIT_TIMEOUT)
    {
        PERROR("WaitForSingleObject");
    }

    if (PARKING2_fin() == -1) {
        PERROR("PARKING2_fin");
        for (int i = 0; i < MAX_HILOS; i++) {
            if (vector_sem_hilo[i] != NULL)
                CloseHandle(vector_sem_hilo[i]);
        }
        UnmapViewOfFile(memoria_parking);
        CloseHandle(zona_memoria);
        CloseHandle(mutex_memoria);
        if (eventoCtrlC != NULL) {
            CloseHandle(eventoCtrlC);
            eventoCtrlC = NULL;
        }
        FreeLibrary(libreria); 
        return -1;
    }

    UnmapViewOfFile(memoria_parking);
    CloseHandle(zona_memoria);
    CloseHandle(mutex_memoria);

    for (int i = 0; i < MAX_HILOS; i++) {
        if (vector_sem_hilo[i] != NULL)
            CloseHandle(vector_sem_hilo[i]);
    }

    if (eventoCtrlC != NULL) {
        CloseHandle(eventoCtrlC);
        eventoCtrlC = NULL;
    }

    if (!FreeLibrary(libreria)) {
        PERROR("FreeLibrary");
        return -1;
    }

    if (fichero_debug != NULL) {
        fclose(fichero_debug);
        fichero_debug = NULL;
    }

    return 0;
}