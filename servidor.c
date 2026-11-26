#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PUERTO 17278  // puerto donde escuchara el servidor
#define TAM_BUFFER 10 // para guardar una peticion o respuesta entera en html
#define MAX_LINEA 516 // protocolo no puede pasar de 516 bytes por linea
int FIN = 0;
void finalizar(){FIN = 1;};
int main(int argc, char *argv[]) {
  int FIN = 0;
  int ls_TCP;
  struct sockaddr_in infoSocket;
  // sigaction
  struct sigaction sa = {.sa_handler = SIG_IGN};
  
  struct sigaction vec = {
    .sa_flags = 0,
    // añadir finalizar
    .sa_handler = finalizar
};

  ls_TCP = socket(AF_INET, SOCK_STREAM, 0);
  if (ls_TCP == -1) {
    perror(argv[0]);
    fprintf(stderr, "%s: no se ha podido crear el socket TCP", argv[0]);
    exit(1);
  }

  // limpio la estructura para configurar donde va el socket
  memset((char *)&infoSocket, 0, sizeof(struct sockaddr_in));
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
  int ls_UTP = socket(AF_INET, SOCK_STREAM, 0);
  if (ls_TCP == -1) {
    perror(argv[0]);
    fprintf(stderr, "%s: no se ha podido crear el socket UDP", argv[0]);
    exit(1);
  }
  if (bind(ls_UTP, (struct sockaddr *)&infoSocket,
           sizeof(struct sockaddr_in) == -1)) {
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
    default:
      exit(0);
    }
  }

  return 0;
}
