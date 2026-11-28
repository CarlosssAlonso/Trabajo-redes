#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PUERTO 17278    // puerto donde escuchara el servidor
#define TAM_BUFFER 2048 // para guardar una peticion o respuesta entera en html
#define MAX_LINEA 516   // protocolo no puede pasar de 516 bytes por linea
int FIN = 0;
int procesarHTTP(char peticion[TAM_BUFFER], char respuesta[TAM_BUFFER]);
void serverUDP(int s, char peticion[TAM_BUFFER],
               struct sockaddr_in clientaddr_in);
void serverTCP(int s, struct sockaddr_in clientaddr_in);
char ip[INET_ADDRSTRLEN];  // cte igual para el max de ip
char hostname[NI_MAXHOST]; // NI_MAXHOST es una cte predefinida de 16 bits que
                           // es el tamaño max del host
void finalizar() { FIN = 1; };
int main(int argc, char *argv[]) {
  fd_set readmask;
  int ls_TCP, s_mayor, num_fds, s_TCP, cc;
  char buffer[TAM_BUFFER];
  struct sockaddr_in infoSocket;
  // sigaction
  socklen_t addrlen;
  struct sockaddr_in clientaddr_in;
  struct sigaction sa = {.sa_handler = SIG_IGN};

  struct sigaction vec = {.sa_flags = 0,
                          // añadir finalizar
                          .sa_handler = finalizar};

  ls_TCP = socket(AF_INET, SOCK_STREAM, 0);
  if (ls_TCP == -1) {
    perror(argv[0]);
    fprintf(stderr, "%s: no se ha podido crear el socket TCP", argv[0]);
    exit(1);
  }

  // limpio la estructura para configurar donde va el socket
  memset((char *)&clientaddr_in, 0, sizeof(struct sockaddr_in));

  memset((char *)&infoSocket, 0, sizeof(struct sockaddr_in));

  addrlen = sizeof(struct sockaddr_in);
  infoSocket.sin_family = AF_INET;
  infoSocket.sin_addr.s_addr = INADDR_ANY;
  // no tiene htons porque es ya 0.0.0.0.0 es una cte de red
  infoSocket.sin_port = htons(PUERTO);

  if (bind(ls_TCP, (const struct sockaddr *)&infoSocket,
           sizeof(struct sockaddr_in)) == -1) {
    perror(argv[0]);
    fprintf(stderr, "%s: no se ha podido reservar el socket al puerto",
            argv[0]);
    exit(1);
  }

  if (listen(ls_TCP, 5) == -1) {
    perror(argv[0]);
    fprintf(stderr, "%s: no se ha podido reservar el socket al puerto",
            argv[0]);
    exit(1);
  }

  // creamos el UDP
  int ls_UDP = socket(AF_INET, SOCK_DGRAM, 0);
  if (ls_UDP == -1) {
    perror(argv[0]);
    fprintf(stderr, "%s: no se ha podido crear el socket UDP", argv[0]);
    exit(1);
  }
  if (bind(ls_UDP, (struct sockaddr *)&infoSocket,
           sizeof(struct sockaddr_in)) == -1) {
    perror(argv[0]);
    fprintf(stderr, "%s: no se ha podido reservar el socket al puerto",
            argv[0]);
    exit(1);
  }

  setpgrp();
  switch (fork()) {
  // si no va el fork
  case -1:
    perror(argv[0]);
    fprintf(stderr, "%s: no se ha podido clonar el servidor", argv[0]);
    exit(1);
  case 0:
    // hijo que trabaja en segundo plano escuchando

    // cierro los procesos de entrada y de salida conectado a los perifericos
    // del sistema
    fclose(stderr);
    fclose(stdin);

    // creamos el sigin para que cuando haya un subhijo que atienda el cliente
    // no quede zombie
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
      perror(argv[0]);
      fprintf(stderr,
              "%s: no se ha podido registrar lo que pasa al finalizar el "
              "hijo que atiende un cliente con SIGCHLD",
              argv[0]);
      exit(1);
    }
    // registro el sigterm si quieren acabar con mi proceso de servidor que no
    // acabe de forma brusca
    if (sigaction(SIGTERM, &vec, (struct sigaction *)0) == -1) {
      perror(argv[0]);
      fprintf(stderr,
              "%s: no se ha podido registrar lo que pasa cuando se envia una "
              "señal de terminacion al proceso DAEMON del servidor",
              argv[0]);
      exit(1);
    }
    while (!FIN) {
      // metemos los sockets UDP y TCP de el hijo en los descriptores de
      // acceso (donde estan las entradas, salidas....) creo un conjunto para
      // definir descriptores de acceso
      FD_ZERO(&readmask);
      FD_SET(ls_TCP, &readmask);
      FD_SET(ls_UDP, &readmask);
      if (ls_TCP > ls_UDP)
        s_mayor = ls_TCP;
      else
        s_mayor = ls_UDP;
      // servidor tiene que esperar hasta que alguno de los dos se vincule tenga
      // peticion de cliente (de todos los sockets a la vez)
      // select regresa cuando algun socket ha cambiado de estado
      if ((num_fds = select(s_mayor + 1, &readmask, (fd_set *)0, (fd_set *)0,
                            NULL)) < 0) {
        // si has recibido alguna señal y ha dado error
        if (num_fds < 0) {
          if (errno != EINTR) {
            perror("Error en select");
          }

          FIN = 1;
        }

      } else {
        // comprobamos quien hace primero la lectura
        // si es el TCP
        if (FD_ISSET(ls_TCP, &readmask)) {

          s_TCP = accept(ls_TCP, (struct sockaddr *)&clientaddr_in, &addrlen);
          // ahora para que no haya una cola de clientes se hace un fork el hijo
          // atiende las peticiones y el padre poermite volver al bucle y seguir
          // atendiendo clientes
          if (s_TCP == -1)
            exit(1);
          switch (fork()) {
          case -1:
            exit(1);
          case 0:
            close(ls_TCP); // cierro el socket de escucha
            close(ls_UDP);
            // logica real de comunicacion tcp
            serverTCP(s_TCP, clientaddr_in);
            exit(0);
          default:
            close(s_TCP); // cierra el socket con el descriptor de socket que
                          // saca al aceptar
          }
        }
        // si ha sido el UDP
        if (FD_ISSET(ls_UDP, &readmask)) {
          // recibe / acepta del cliente
          cc = recvfrom(ls_UDP, buffer, TAM_BUFFER - 1, 0,
                        (struct sockaddr *)&clientaddr_in, &addrlen);
          if (cc == -1) {
            perror(argv[0]);
            printf("%s: error en la aceptacion del cliente usando udp",
                   argv[0]);
            exit(1);
          }
          // pone un \0 como si fuera una cadena el buffer y el cc es el ultimo
          // valor de la cadena
          buffer[cc] = '\0';
          // logica UDP
          serverUDP(ls_UDP, buffer, clientaddr_in);
        }
      }
    }
    close(ls_TCP);
    close(ls_UDP);
    printf("\nFinaliza la conexión del servidor");

  default:
    exit(0);
  }

  return 0;
}

void serverTCP(int s, struct sockaddr_in clientaddr_in) {
  // lo primero era escribrir en el fichero los datos de la conexion
  inet_ntop(
      AF_INET, &clientaddr_in.sin_addr, ip,
      sizeof(
          ip)); // la ip esta en binario tenemos que traducirla en el vector ip
  getnameinfo((struct sockaddr *)&clientaddr_in, sizeof(clientaddr_in),
              hostname, sizeof(hostname), NULL, 0,
              0); // guardamos el nombre del cliente
  // ya tenemos nombre + ip es decir al cliente identificado
  time_t tiempoActual;
  time(&tiempoActual);
  int puerto = ntohs(
      clientaddr_in
          .sin_port); // traducimos el puerto de bytes de red a maquina propia
  FILE *log = fopen("peticiones.log", "a"); // ahora escribimos en el fichero
  if (log != NULL) {
    fprintf(log, "%.24s %s %s TCP %d\n", ctime(&tiempoActual), hostname, ip,
            puerto);
    fclose(log);
  }
  struct linger linger_cierre_seguro;
  linger_cierre_seguro.l_linger = 5; // tiempo maximo de espera
  linger_cierre_seguro.l_onoff = 1; // activamos el linger para el cierre seguro
  if (setsockopt(s, SOL_SOCKET, SO_LINGER, &linger_cierre_seguro,
            sizeof(linger_cierre_seguro)) < 0) {
    perror("Error al configurar el linger en el socket");
  } // lo aplicamos a la configuracion

  // logica
  int cerrar = 0;
  while (!cerrar) {
    // descriptores de acceso
    fd_set readmask;
    FD_ZERO(&readmask);
    FD_SET(s, &readmask);

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

    int resultado = select(s + 1, &readmask, NULL, NULL, &timeout);
    if (resultado <= 0) {
      if (resultado < 0) {
        perror("Error en la espera de datos");
      }

      cerrar = 1;
    }
    char peticion[TAM_BUFFER];
    peticion[0] = '\0';

    int total = 0;
    ssize_t bytes;

    while (!cerrar && strstr(peticion, "\r\n\r\n") == NULL) {

      bytes = recv(s, peticion + total, sizeof(peticion) - total - 1, 0);

      if (bytes <= 0) {
        cerrar = 1;
      } else {
        total += bytes;
        peticion[total] = '\0';

        if (total == sizeof(peticion) - 1 &&
            strstr(peticion, "\r\n\r\n") == NULL) {
          cerrar = 1;
        }
      }
    }
    // ahora tenemos que enviar la respuesta
    if (!cerrar) {
      char respuesta[TAM_BUFFER];
      int len_respuesta = procesarHTTP(peticion, respuesta);
      if (len_respuesta <= 0) {
        cerrar = 1;
      } else {
        int enviados = 0;
        while (!cerrar && enviados < len_respuesta) {
          bytes = send(s, respuesta + enviados, len_respuesta - enviados, 0);
          if (bytes <= 0) {
            cerrar = 1;
          } else {
            enviados += bytes;
          }
        }
        // si debo cerrar la conexion (cliente cierra conexion o se recibe 501)
        // o seguir esperando para recibir mas peticiones
        if (strstr(peticion, "Connection: close") != NULL ||
            strncmp(respuesta, "HTTP/1.1 501", 12) == 0) {
          cerrar = 1;
        }
      }

      // Enviar respuesta
      // Comprobar si hay que cerrar
    }
  }
  close(s);
}

int procesarHTTP(char peticion[TAM_BUFFER], char respuesta[TAM_BUFFER]) {
  char metodo[MAX_LINEA];
  char recurso[MAX_LINEA] = "";
  char version[MAX_LINEA];
  char ruta[MAX_LINEA + 10];

  char cuerpo[TAM_BUFFER];
  char *estado;

  int contador = 0;
  int cerrar = 0;
  int longitud_cuerpo = 0;
  int longitud_cabecera = 0;

  for (int i = 0; peticion[i] != '\0'; i++) {
    contador++;

    if (contador > MAX_LINEA) {
      return -1;
    }

    if (peticion[i] == '\n') {
      if (i == 0 || peticion[i - 1] != '\r') {
        return -1;
      }

      contador = 0;
    }
  }

  if (strstr(peticion, "Connection: close") != NULL) {
    cerrar = 1;
  }
  // divide la peticion segun cada campo
  int campos = sscanf(peticion, "%515s %515s %515s", metodo, recurso, version);
  // si hay algun error en la peticion si no es la misma version http
  if (campos != 3 || strcmp(metodo, "GET") != 0 ||
      strcmp(version, "HTTP/1.1") != 0) {

    estado = "501 Not Implemented";

    strcpy(cuerpo, "<html><body><h1>501 Not Implemented</h1></body></html>");

    longitud_cuerpo = strlen(cuerpo);
    cerrar = 1;

  } else {

    FILE *archivo = NULL;
    // es el formato que queremos se abre segun la ruta
    if (recurso[0] == '/' && strstr(recurso, "..") == NULL) {

      snprintf(ruta, sizeof(ruta), "./www%s", recurso);

      archivo = fopen(ruta, "rb");
    }
    // si no existe el archivo
    if (archivo == NULL) {

      estado = "404 Not Found";

      strcpy(cuerpo, "<html><body><h1>404 Not Found</h1></body></html>");

      longitud_cuerpo = strlen(cuerpo);

    } else {

      estado = "200 OK";
      longitud_cuerpo = fread(cuerpo, 1, sizeof(cuerpo), archivo);
      // si hay algun error en la lectura del archivo
      if (ferror(archivo) || !feof(archivo)) {
        fclose(archivo);
        return -1;
      }

      fclose(archivo);
    }
  }

  if (cerrar) {

    longitud_cabecera = snprintf(respuesta, TAM_BUFFER,
                                 "HTTP/1.1 %s\r\n"
                                 "Server: Servidor de Nombre_alumno\r\n"
                                 "Connection: close\r\n"
                                 "Content-Length: %d\r\n"
                                 "\r\n",
                                 estado, longitud_cuerpo);

  } else {

    longitud_cabecera = snprintf(respuesta, TAM_BUFFER,
                                 "HTTP/1.1 %s\r\n"
                                 "Server: Servidor de Nombre_alumno\r\n"
                                 "Connection: keep-alive\r\n"
                                 "Keep-Alive: timeout=5\r\n"
                                 "Content-Length: %d\r\n"
                                 "\r\n",
                                 estado, longitud_cuerpo);
  }

  if (longitud_cabecera < 0 || longitud_cabecera >= TAM_BUFFER ||
      longitud_cuerpo > TAM_BUFFER - longitud_cabecera) {
    return -1;
  }

  memcpy(respuesta + longitud_cabecera, cuerpo, longitud_cuerpo);

  FILE *log = fopen("peticiones.log", "a");

  if (log != NULL) {
    fprintf(log, "%s - %s\n", recurso, estado);
    fclose(log);
  }

  return longitud_cabecera + longitud_cuerpo;
}
void serverUDP(int s, char peticion[TAM_BUFFER],
               struct sockaddr_in clientaddr_in) {
  inet_ntop(AF_INET, &clientaddr_in.sin_addr, ip, sizeof(ip));

  if (getnameinfo((struct sockaddr *)&clientaddr_in, sizeof(clientaddr_in),
                  hostname, sizeof(hostname), NULL, 0, 0) != 0) {
    strcpy(hostname, ip);
  }

  time_t tiempoActual;
  time(&tiempoActual);

  int puerto = ntohs(clientaddr_in.sin_port);

  FILE *log = fopen("peticiones.log", "a");

  if (log != NULL) {
    fprintf(log, "%.24s %s %s UDP %d\n", ctime(&tiempoActual), hostname, ip,
            puerto);

    fclose(log);
  }

  char respuesta[TAM_BUFFER];

  int len_respuesta = procesarHTTP(peticion, respuesta);

  if (len_respuesta > 0) {
    if (sendto(s, respuesta, len_respuesta, 0,
               (struct sockaddr *)&clientaddr_in,
               sizeof(clientaddr_in)) == -1) {
      perror("Error al enviar la respuesta UDP");
    }
  }
}
