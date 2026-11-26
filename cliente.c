#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define PUERTO      17278   /* el mismo que el servidor */
#define TAM_BUFFER  4096
#define MAX_LINEA   516
#define TIMEOUT     5       /* UDP: segundos esperando respuesta */
#define REINTENTOS  3       /* UDP: veces que se reenvía */

/* Rellena servaddr con la IP de 'nombre' (getaddrinfo) y htons(PUERTO).
   Devuelve 0 si va bien, -1 si no se puede resolver. */
int resolver_servidor(char *nombre, struct sockaddr_in *servaddr);

/* Todo el cliente TCP: socket, connect, getsockname, abrir <puerto>.txt,
   bucle de órdenes (send + leer respuesta) y cierre (shutdown + close). */
void clienteTCP(char *servidor, struct sockaddr_in servaddr, char *fich_ordenes);

/* Todo el cliente UDP: socket, bind al puerto 0, getsockname, abrir <puerto>.txt,
   bucle de órdenes (sendto + recvfrom con alarm y reintentos) y close. */
void clienteUDP(char *servidor, struct sockaddr_in servaddr, char *fich_ordenes);

/* Convierte una línea de ordenes.txt ("GET /index.html k") en la petición HTTP
   completa, acabada en la línea vacía. Pone *cerrar = 1 si la orden lleva 'c'.
   Devuelve -1 si la línea está vacía o mal formada. */
int construir_peticion(char *linea, char *servidor, char *peticion, int *cerrar);

/* TCP: lee del socket una línea acabada en \r\n, byte a byte, y la deja sin el \r\n.
   Devuelve su longitud (0 = línea vacía = fin de cabeceras) o -1 si el servidor cerró. */
int leer_linea(int s, char *linea, int max);

/* Manejador de SIGALRM: vacío. Solo sirve para interrumpir el recvfrom (EINTR). */
void manejador_alarma(int sig);


int main(int argc, char *argv[])
{
    struct sockaddr_in servaddr;

    /* 1. Argumentos: cliente <servidor> <TCP|UDP> <fichero_ordenes> */
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <servidor> <TCP|UDP> <fichero_ordenes>\n", argv[0]);
        exit(1);
    }

    /* 2. Nombre del servidor -> IP + puerto */
    if (resolver_servidor(argv[1], &servaddr) == -1) {
        fprintf(stderr, "%s: no se pudo resolver %s\n", argv[0], argv[1]);
        exit(1);
    }

    /* 3. Cada protocolo por su camino */
    if (strcmp(argv[2], "TCP") == 0)
        clienteTCP(argv[1], servaddr, argv[3]);
    else if (strcmp(argv[2], "UDP") == 0)
        clienteUDP(argv[1], servaddr, argv[3]);
    else {
        fprintf(stderr, "%s: el protocolo debe ser TCP o UDP\n", argv[0]);
        exit(1);
    }

    return 0;
}
