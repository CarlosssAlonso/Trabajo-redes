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

/* ---------- Prototipos (arriba, para que el orden de las funciones dé igual) ---------- */
int  resolver_servidor(char *nombre, struct sockaddr_in *servaddr);
void clienteTCP(char *servidor, struct sockaddr_in servaddr, char *fich_ordenes);
int  conectar_tcp(struct sockaddr_in servaddr);
void clienteUDP(char *servidor, struct sockaddr_in servaddr, char *fich_ordenes);
int  construir_peticion(char *linea, char *servidor, char *peticion, int *cerrar);
int  leer_linea(int s, char *linea, int max);
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

/* Rellena servaddr con la IP de 'nombre' (getaddrinfo) y htons(PUERTO).
   Devuelve 0 si va bien, -1 si no se puede resolver. */
int resolver_servidor(char *nombre, struct sockaddr_in *servaddr)
{
    struct addrinfo hints, *res;

    /* 1. Estructura destino a cero (sizeof(*servaddr): la estructura, no el puntero) */
    memset(servaddr, 0, sizeof(*servaddr));
    servaddr->sin_family = AF_INET;

    /* 2. La "pregunta": solo direcciones IPv4 */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;

    /* 3. Traducir el nombre a IP */
    if (getaddrinfo(nombre, NULL, &hints, &res) != 0)
        return -1;

    /* 4. Copiar la IP del primer resultado (cast de sockaddr genérico a sockaddr_in) */
    servaddr->sin_addr = ((struct sockaddr_in *) res->ai_addr)->sin_addr;

    /* 5. Liberar la lista que reservó getaddrinfo (después de copiar) */
    freeaddrinfo(res);

    /* 6. Puerto del servidor en orden de red */
    servaddr->sin_port = htons(PUERTO);

    return 0;
}

/* Crea un socket TCP y lo conecta con el servidor.
   Devuelve el socket, o -1 si no se ha podido. */
int conectar_tcp(struct sockaddr_in servaddr)
{
    int s;

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return -1;
    if (connect(s, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
        close(s);
        return -1;
    }
    return s;
}

/* Todo el cliente TCP: conectar, getsockname, abrir <puerto>.txt,
   bucle de órdenes (send + leer respuesta) y cierre (shutdown + close).
   Si el servidor cierra la conexión (Connection: close, 501...) y quedan
   órdenes en el fichero, se vuelve a conectar para seguir con ellas. */
void clienteTCP(char *servidor, struct sockaddr_in servaddr, char *fich_ordenes)
{
    int s;
    struct sockaddr_in myaddr;
    socklen_t addrlen = sizeof(myaddr);
    char nombre_salida[32];
    FILE *fsal, *ford;
    char linea[MAX_LINEA + 2];
    char peticion[TAM_BUFFER];
    char cabecera[MAX_LINEA + 1];
    char cuerpo[TAM_BUFFER];
    int cerrar, conexion_rota;
    int longitud, leidos, n, r;

    /* 1. Crear el socket TCP y conectar (saludo en 3 pasos) */
    if ((s = conectar_tcp(servaddr)) == -1) {
        perror("connect");
        exit(1);
    }

    /* 2. Puerto efímero: después del connect */
    if (getsockname(s, (struct sockaddr *)&myaddr, &addrlen) == -1) {
        perror("getsockname");
        exit(1);
    }

    /* 3. Abrir el fichero de salida <puerto>.txt y el de órdenes */
    sprintf(nombre_salida, "%u.txt", ntohs(myaddr.sin_port));
    if ((fsal = fopen(nombre_salida, "w")) == NULL) {
        perror("fopen");
        exit(1);
    }
    if ((ford = fopen(fich_ordenes, "r")) == NULL) {
        fprintf(fsal, "Error: no se puede abrir el fichero de ordenes %s\n", fich_ordenes);
        fclose(fsal);
        close(s);
        exit(1);
    }
    fprintf(fsal, "Conectado a %s (%s:%d) por TCP desde el puerto %u\n\n",
            servidor, inet_ntoa(servaddr.sin_addr), ntohs(servaddr.sin_port),
            ntohs(myaddr.sin_port));

    /* 4. Bucle de órdenes: una vuelta = una petición y su respuesta */
    while (fgets(linea, sizeof(linea), ford) != NULL) {

        /* 4.1 Traducir la orden a petición HTTP */
        if (construir_peticion(linea, servidor, peticion, &cerrar) == -1)
            continue;                               /* línea vacía: siguiente */

        /* 4.2 Si la conexión anterior se cerró, abrir una nueva */
        if (s == -1) {
            if ((s = conectar_tcp(servaddr)) == -1) {
                fprintf(fsal, "Error: no se puede volver a conectar con el servidor\n");
                break;
            }
            addrlen = sizeof(myaddr);
            getsockname(s, (struct sockaddr *)&myaddr, &addrlen);
            fprintf(fsal, "--- Nueva conexion TCP desde el puerto %u ---\n\n",
                    ntohs(myaddr.sin_port));
        }

        fprintf(fsal, "Cliente> %s\n", linea);

        /* 4.3 Enviarla (strlen, no sizeof) */
        conexion_rota = 0;
        if (send(s, peticion, strlen(peticion), 0) == -1) {
            fprintf(fsal, "Error al enviar la peticion\n");
            conexion_rota = 1;
        }

        /* 4.4 Leer las cabeceras hasta la línea vacía */
        longitud = 0;
        while (!conexion_rota && (r = leer_linea(s, cabecera, sizeof(cabecera))) > 0) {
            fprintf(fsal, "%s\n", cabecera);
            if (strncmp(cabecera, "Content-Length:", 15) == 0)
                longitud = atoi(cabecera + 15);
            if (strcmp(cabecera, "Connection: close") == 0)
                cerrar = 1;
        }
        if (!conexion_rota && r == -1) {
            fprintf(fsal, "El servidor ha cerrado la conexion\n");
            conexion_rota = 1;
        }

        /* 4.5 Leer el cuerpo: exactamente 'longitud' bytes */
        if (!conexion_rota) {
            fprintf(fsal, "\n");
            if (longitud > TAM_BUFFER - 1)
                longitud = TAM_BUFFER - 1;
            leidos = 0;
            while (leidos < longitud) {
                n = recv(s, cuerpo + leidos, longitud - leidos, 0);
                if (n
