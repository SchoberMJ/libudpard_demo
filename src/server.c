/*
        Simple udp server
*/
#include <arpa/inet.h>
#include <stdio.h>  //printf
#include <stdlib.h> //exit(0);
#include <string.h> //memset
#include <sys/socket.h>

// #define MULTICAST_GROUP "239.255.255.250"
#define MULTICAST_GROUP "239.0.9.42"
// #define MULTICAST_GROUP "239.0.4.210"
// #define MULTICAST_GROUP "255.255.255.250"
#define MULTICAST_GROUP_INTERFACE "239.0.0.24"
#define BUFLEN 512 // Max length of buffer
#define PORT 16383 // The port on which to listen for incoming data

void die(char *s) {
  perror(s);
  exit(1);
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    die("Expected an address");
  }
  char *interface = argv[1];
  struct sockaddr_in si_me, si_other;
  struct sockaddr_in remote_addr;

  int remote_socket, s, i, slen = sizeof(si_other), recv_len;
  char buf[BUFLEN];

  // create a UDP socket
  if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
    die("socket");
  }

  u_int yes = 1;
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes)) < 0) {
    die("Reusing ADDR failed");
  }
  // zero out the structure
  memset((char *)&si_me, 0, sizeof(si_me));

  si_me.sin_family = AF_INET;
  si_me.sin_port = htons(PORT);
  // si_me.sin_addr.s_addr = htonl(INADDR_ANY);
  si_me.sin_addr.s_addr = inet_addr(MULTICAST_GROUP);

  // bind socket to port
  if (bind(s, (struct sockaddr *)&si_me, sizeof(si_me)) == -1) {
    die("bind");
  }

  struct ip_mreq group;
  group.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
  // group.imr_interface.s_addr = inet_addr(MULTICAST_GROUP_INTERFACE);
  group.imr_interface.s_addr = inet_addr(interface);
  // group.imr_interface.s_addr = htonl(INADDR_ANY);
  if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&group,
                 sizeof(group)) < 0) {
    die("setsockopt membership");
  }

  // Setup the remote socket for response
  if ((remote_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
    die("socket");
  }
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons(PORT);
  remote_addr.sin_addr.s_addr = inet_addr(interface);

  // bind socket to port
  if (bind(remote_socket, (struct sockaddr *)&remote_addr,
           sizeof(remote_addr)) == -1) {
    die("bind");
  }

  // keep listening for data
  while (1) {
    printf("Waiting for data...");
    fflush(stdout);

    // try to receive some data, this is a blocking call
    if ((recv_len = recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *)&si_other,
                             &slen)) == -1) {
      die("recvfrom()");
    }

    // print details of the client/peer and the data received
    printf("Received packet from %s:%d\n", inet_ntoa(si_other.sin_addr),
           ntohs(si_other.sin_port));
    printf("Data: %s\n", buf);
    printf("Read size = %d\n", recv_len);
    for (int i = 0; i < recv_len; i++) {
      printf("%02X ", buf[i]);
    }
    printf("\n");

    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(PORT);
    remote_addr.sin_addr.s_addr = si_other.sin_addr.s_addr;
    // now reply the client with the same data
    if (sendto(remote_socket, buf, recv_len, 0, (struct sockaddr *)&remote_addr,
               slen) == -1) {
      die("sendto()");
    }
    // print details of the client/peer and the data received
    printf("Sent packet to %s:%d\n", inet_ntoa(remote_addr.sin_addr),
           ntohs(remote_addr.sin_port));
    printf("Data: %s\n", buf);
  }

  close(s);
  return 0;
}
