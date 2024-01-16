//--------------------------------------------------------------------------------------------------
// Network Lab                               Fall 2023                           System Programming
//
/// @file
/// @brief Client-side implementation of Network Lab
///
/// @author hyunwoo LEE
/// @studid 2020-12907
///
/// @section changelog Change Log
/// 2020/11/18 Hyunik Kim created
/// 2021/11/23 Jaume Mateu Cuadrat cleanup, add milestones
///
/// @section license_section License
/// Copyright (c) 2020-2023, Computer Systems and Platforms Laboratory, SNU
/// All rights reserved.
///
/// Redistribution and use in source and binary forms, with or without modification, are permitted
/// provided that the following conditions are met:
///
/// - Redistributions of source code must retain the above copyright notice, this list of condi-
///   tions and the following disclaimer.
/// - Redistributions in binary form must reproduce the above copyright notice, this list of condi-
///   tions and the following disclaimer in the documentation and/or other materials provided with
///   the distribution.
///
/// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
/// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED  TO, THE IMPLIED  WARRANTIES OF MERCHANTABILITY
/// AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
/// CONTRIBUTORS  BE LIABLE FOR ANY DIRECT,  INDIRECT, INCIDENTAL, SPECIAL,  EXEMPLARY,  OR CONSE-
/// QUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
/// LOSS OF USE, DATA,  OR PROFITS; OR BUSINESS INTERRUPTION)  HOWEVER CAUSED AND ON ANY THEORY OF
/// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
/// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
/// DAMAGE.
//--------------------------------------------------------------------------------------------------

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "net.h"
#include "burger.h"

/// @brief client error function
/// @param socketfd file drescriptor of the socket
void error_client(int socketfd) {
	close(socketfd);
  	pthread_exit(NULL);
}

/// @brief client task for connection thread
void *thread_task(void *data)
{
  struct addrinfo *ai, *ai_it;
  size_t read, sent, buflen;
  int serverfd = -1, choice, ret;
  char *buffer;
  pthread_t tid;

  tid = pthread_self();

  buffer = (char *)malloc(BUF_SIZE);
  buflen = BUF_SIZE;

  // Get the socket list
  int r;
  ai = getsocklist(IP, PORT, AF_UNSPEC, SOCK_STREAM, 0, &r);
  if (ai == NULL) {
    fprintf(stderr, "getsocklist() failed: %s\n", gai_strerror(r));
    pthread_exit(NULL);
  }

  // Create a socket and connect
  ai_it = ai;
  while (ai_it != NULL) {
    serverfd = socket(ai_it->ai_family, ai_it->ai_socktype, ai_it->ai_protocol);
    if (serverfd == -1) {
      ai_it = ai_it->ai_next;
      continue;
    }
    if (connect(serverfd, ai_it->ai_addr, ai_it->ai_addrlen) != -1) {
      break;
    }
    close(serverfd);
    ai_it = ai_it->ai_next;
  }

  // If connecting to server fails, print error message and exit
  if (ai_it == NULL) {
    fprintf(stderr, "Unable to connect to server\n");
    freeaddrinfo(ai);
    pthread_exit(NULL);
  }

  // Read welcome message from the server
  read = get_line(serverfd, &buffer, &buflen);
  if (read <= 0) {
    printf("Cannot read data from server\n");
    error_client(serverfd);
  }

  printf("[Thread %lu] From server: %s", tid, buffer);

  // Randomly choose which burger to order
  choice = rand() % BURGER_TYPE_MAX;
  ret = strlen(burger_names[choice]);
  printf("[Thread %lu] To server: Can I have a %s burger?\n", tid, burger_names[choice]);

  memset(buffer, 0, BUF_SIZE);
  strncpy(buffer, burger_names[choice], ret);
  buffer[ret] = '\n';

  // Send burger name to the server
  sent = put_line(serverfd, buffer, strlen(buffer));
  if (sent < 0) {
    printf("Error: cannot send data to server\n");
    error_client(serverfd);
  }

  // Get final message from the server
  memset(buffer, 0, BUF_SIZE);
  read = get_line(serverfd, &buffer, &buflen);
  if (read <= 0) {
    printf("Cannot read data from server\n");
    error_client(serverfd);
  }

  printf("[Thread %lu] From server: %s", tid, buffer);

  close(serverfd);
  freeaddrinfo(ai);
  pthread_exit(NULL);
}

/// @brief program entry point
int main(int argc, char const *argv[])
{
  int i;
  int num_threads;
  pthread_t *threads;

  if (argc != 2) {
    printf("usage ./client <num_threads>\n");
    return 0;
  }

  // Get number of threads
  num_threads = atoi(argv[1]);
  if (num_threads <= 0) {
    printf("Invalid number of threads. Must be a positive integer.\n");
    return 1;
  }

  // Malloc for threads array
  threads = malloc(num_threads * sizeof(pthread_t));
  if (threads == NULL) {
    perror("Failed to allocate memory for threads");
    return 1;
  }

  // Generate threads
  for (i = 0; i < num_threads; i++) {
    if (pthread_create(&threads[i], NULL, thread_task, NULL) != 0) {
      perror("Failed to create thread");
      free(threads);
      return 1;
    }
  }

  // Join threads, and wait for all threads' work done
  for (int i = 0; i < num_threads; i++) {
    pthread_join(threads[i], NULL);
  }

  // Free threads array
  free(threads);

  return 0;
}
