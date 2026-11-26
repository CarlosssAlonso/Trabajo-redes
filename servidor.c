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
void finalizar() { FIN = 1; };
int main(int argc, char *argv[]) {
  fd_set readmask;
  int ls_TCP, s_mayor, num_fds, s_TCP, cc, s_UDP;
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
        if (errno == EINTR) {
          FIN = 1;
          close(ls_TCP);
          close(ls_UDP);
          perror("El servidor se cierra por un error a la hora de esperar una "
                 "peticion");
        }

      } else {
        // comprobamos quien hace primero la lectura
        // si es el TCP
        if (FD_ISSET(ls_TCP, &readmask)) {
         
          s_TCP = accept(ls_TCP, (struct sockaddr *)&clientaddr_in, &addrlen);
          // ahora para que no haya una cola de clientes se hace un fork el hijo atiende las peticiones y el padre poermite volver al bucle y seguir atendiendo clientes
          if(s_TCP==-1) exit(1);
          switch (fork()) {
              case -1:
                exit(1);
              case 0:
              close(ls_TCP); // cierro el socket de escucha
              // logica real de comunicacion tcp
              exit(0);
              default:
              close(s_TCP); // cierra el socket con el descriptor de socket que saca al aceptar

          }
        }
        // si ha sido el UDP
        if (FD_ISSET(ls_UDP, &readmask)) 
        {
          // recibe / acepta del cliente
          cc = recvfrom(s_UDP, buffer, TAM_BUFFER -1, 0, (struct sockaddr *)&clientaddr_in, &addrlen);
          if(cc == -1)
          {
            perror(argv[0]);
            printf("%s: error en la aceptacion del cliente usando udp", argv[0]);
            exit(1); 
          }
          //pone un \0 como si fuera una cadena el buffer y el cc es el ultimo valor de la cadena
          buffer[cc]='\0';
          // logica UDP
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
