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

/* Todo el cliente TCP: socket, connect, getsockname, abrir <puerto>.txt,
   bucle de órdenes (send + leer respuesta) y cierre (shutdown + close). */
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
    int cerrar, fin = 0;
    int longitud, leidos, n, r;

    /* 1. Crear el socket TCP */
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }

    /* 2. Conectar con el servidor (aquí se hace el saludo en 3 pasos) */
    if (connect(s, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
        perror("connect");
        exit(1);
    }

    /* 3. Puerto efímero: después del connect */
    if (getsockname(s, (struct sockaddr *)&myaddr, &addrlen) == -1) {
        perror("getsockname");
        exit(1);
    }

    /* 4. Abrir el fichero de salida <puerto>.txt y el de órdenes */
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

    /* 5. Bucle de órdenes: una vuelta = una petición y su respuesta */
    while (!fin && fgets(linea, sizeof(linea), ford) != NULL) {

        /* 5.1 Traducir la orden a petición HTTP */
        if (construir_peticion(linea, servidor, peticion, &cerrar) == -1)
            continue;                               /* línea vacía: siguiente */
        fprintf(fsal, "Cliente> %s\n", linea);

        /* 5.2 Enviarla (strlen, no sizeof) */
        if (send(s, peticion, strlen(peticion), 0) == -1) {
            fprintf(fsal, "Error al enviar la peticion\n");
            break;
        }

        /* 5.3 Leer las cabeceras hasta la línea vacía */
        longitud = 0;
        while ((r = leer_linea(s, cabecera, sizeof(cabecera))) > 0) {
            fprintf(fsal, "%s\n", cabecera);
            if (strncmp(cabecera, "Content-Length:", 15) == 0)
                longitud = atoi(cabecera + 15);
            if (strcmp(cabecera, "Connection: close") == 0)
                cerrar = 1;
        }
        if (r == -1) {
            fprintf(fsal, "El servidor ha cerrado la conexion\n");
            break;
        }
        fprintf(fsal, "\n");

        /* 5.4 Leer el cuerpo: exactamente 'longitud' bytes */
        if (longitud > TAM_BUFFER - 1)
            longitud = TAM_BUFFER - 1;
        leidos = 0;
        while (leidos < longitud) {
            n = recv(s, cuerpo + leidos, longitud - leidos, 0);
            if (n <= 0) {
                fprintf(fsal, "El servidor ha cerrado antes de enviar todo el cuerpo\n");
                fin = 1;
                break;
            }
            leidos += n;
        }
        cuerpo[leidos] = '\0';
        fprintf(fsal, "%s\n\n", cuerpo);
        fflush(fsal);

        /* 5.5 ¿Seguir? Tras un Connection: close (501 u orden con 'c') no */
        if (cerrar)
            fin = 1;
    }

    /* 6. Finalización ordenada */
    fprintf(fsal, "Fin de la comunicacion\n");
    shutdown(s, SHUT_WR);       /* "no te mando nada más" */
    close(s);
    fclose(ford);
    fclose(fsal);
}

/* Todo el cliente UDP: socket, bind al puerto 0, getsockname, abrir <puerto>.txt,
   bucle de órdenes (sendto + recvfrom con alarm y reintentos) y close. */
void clienteUDP(char *servidor, struct sockaddr_in servaddr, char *fich_ordenes)
{
    int s;
    struct sockaddr_in myaddr, from;
    socklen_t addrlen = sizeof(myaddr);
    socklen_t fromlen;
    struct sigaction vec;
    char nombre_salida[32];
    FILE *fsal, *ford;
    char linea[MAX_LINEA + 2];
    char peticion[TAM_BUFFER];
    char buffer[TAM_BUFFER];
    int cerrar, intento, recibido, cc;

    /* 1. Crear el socket UDP */
    if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }

    /* 2. bind a cualquier IP local y puerto 0: el sistema elige el efímero */
    memset(&myaddr, 0, sizeof(myaddr));
    myaddr.sin_family = AF_INET;
    myaddr.sin_addr.s_addr = INADDR_ANY;
    myaddr.sin_port = 0;
    if (bind(s, (struct sockaddr *)&myaddr, sizeof(myaddr)) == -1) {
        perror("bind");
        exit(1);
    }

    /* 3. Puerto efímero elegido */
    if (getsockname(s, (struct sockaddr *)&myaddr, &addrlen) == -1) {
        perror("getsockname");
        exit(1);
    }

    /* 4. SIGALRM para el timeout. Sin SA_RESTART: así recvfrom
          devuelve -1 con errno == EINTR cuando salta la alarma */
    memset(&vec, 0, sizeof(vec));
    vec.sa_handler = manejador_alarma;
    vec.sa_flags = 0;
    if (sigaction(SIGALRM, &vec, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    /* 5. Abrir ficheros */
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
    fprintf(fsal, "Cliente UDP hacia %s (%s:%d) desde el puerto %u\n\n",
            servidor, inet_ntoa(servaddr.sin_addr), ntohs(servaddr.sin_port),
            ntohs(myaddr.sin_port));

    /* 6. Bucle de órdenes */
    while (fgets(linea, sizeof(linea), ford) != NULL) {

        /* 6.1 Traducir la orden */
        if (construir_peticion(linea, servidor, peticion, &cerrar) == -1)
            continue;
        fprintf(fsal, "Cliente> %s\n", linea);

        /* 6.2 Enviar y esperar respuesta, con reintentos */
        recibido = 0;
        for (intento = 1; intento <= REINTENTOS && !recibido; intento++) {

            if (sendto(s, peticion, strlen(peticion), 0,
                       (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
                fprintf(fsal, "Error al enviar la peticion\n");
                break;
            }

            alarm(TIMEOUT);
            fromlen = sizeof(from);
            cc = recvfrom(s, buffer, TAM_BUFFER - 1, 0,
                          (struct sockaddr *)&from, &fromlen);

            if (cc == -1) {
                if (errno == EINTR) {           /* saltó la alarma */
                    fprintf(fsal, "Timeout: sin respuesta (intento %d de %d)\n",
                            intento, REINTENTOS);
                    continue;                   /* reintentar */
                }
                alarm(0);
                fprintf(fsal, "Error en recvfrom\n");
                break;
            }

            alarm(0);                           /* ha llegado: cancelar alarma */
            recibido = 1;
        }

        if (!recibido) {
            fprintf(fsal, "Sin respuesta del servidor\n\n");
            fflush(fsal);
            continue;                           /* siguiente orden */
        }

        /* 6.3 Guardar la respuesta (llega entera en un datagrama) */
        buffer[cc] = '\0';
        fprintf(fsal, "%s\n\n", buffer);
        fflush(fsal);

        /* 6.4 ¿Seguir? */
        if (strstr(buffer, "Connection: close") != NULL)
            cerrar = 1;
        if (cerrar)
            break;
    }

    /* 7. Fin (en UDP no hay shutdown: no hay conexión) */
    fprintf(fsal, "Fin de la comunicacion\n");
    close(s);
    fclose(ford);
    fclose(fsal);
}

/* Convierte una línea de ordenes.txt ("GET /index.html k") en la petición HTTP
   completa, acabada en la línea vacía. Pone *cerrar = 1 si la orden lleva 'c'.
   Devuelve -1 si la línea está vacía o mal formada. */
int construir_peticion(char *linea, char *servidor, char *peticion, int *cerrar)
{
    char metodo[MAX_LINEA], ruta[MAX_LINEA], flag[8];
    int n;

    *cerrar = 0;

    /* 1. Quitar el \n (y el \r de Windows) del final */
    linea[strcspn(linea, "\r\n")] = '\0';

    /* 2. Separar la línea en sus palabras */
    n = sscanf(linea, "%s %s %7s", metodo, ruta, flag);
    if (n < 2)
        return -1;                       /* línea vacía o incompleta */

    /* 3. Primera línea + Host */
    sprintf(peticion, "%s %s HTTP/1.1\r\nHost: %s\r\n", metodo, ruta, servidor);

    /* 4. Cabecera Connection según el flag */
    if (n == 3 && strcmp(flag, "k") == 0)
        strcat(peticion, "Connection: keep-alive\r\n");
    else if (n == 3 && strcmp(flag, "c") == 0) {
        strcat(peticion, "Connection: close\r\n");
        *cerrar = 1;
    }

    /* 5. Línea vacía: fin de la petición */
    strcat(peticion, "\r\n");
    return 0;
}

/* TCP: lee del socket una línea acabada en \r\n, byte a byte, y la deja sin el \r\n.
   Devuelve su longitud (0 = línea vacía = fin de cabeceras) o -1 si el servidor cerró. */
int leer_linea(int s, char *linea, int max)
{
    char c;
    int i = 0;
    int n;

    while (1) {
        n = recv(s, &c, 1, 0);      /* leer UN byte */
        if (n <= 0)
            return -1;              /* servidor cerró (0) o error (-1) */
        if (c == '\n')
            break;                  /* fin de línea */
        if (i < max - 1)
            linea[i++] = c;         /* guardarlo (sin desbordar) */
    }

    if (i > 0 && linea[i - 1] == '\r')
        i--;                        /* quitar el \r del final */

    linea[i] = '\0';                /* cerrar la cadena */
    return i;                       /* longitud: 0 = línea vacía */
}

/* Manejador de SIGALRM: vacío. Solo sirve para interrumpir el recvfrom (EINTR). */
void manejador_alarma(int sig)
{
    (void)sig;   /* no hace nada: solo sirve para interrumpir el recvfrom */
}
