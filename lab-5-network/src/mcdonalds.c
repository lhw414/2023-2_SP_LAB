//--------------------------------------------------------------------------------------------------
// Network Lab                               Fall 2023                           System Programming
//
/// @file
/// @brief Simple virtual McDonald's server for Network Lab
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
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

#include "net.h"
#include "burger.h"

/// @name Constant definitions
/// @{

#define CUSTOMER_MAX 20                                     ///< maximum number of clients
#define NUM_KITCHEN 5                                       ///< number of kitchen thread(s)

/// @}

/// @name Structures
/// @{

/// @brief general node element to implement a singly-linked list
typedef struct __node {
  struct __node *next;                                      ///< pointer to next node
  unsigned int customerID;                                  ///< customer ID that requested
  enum burger_type type;                                    ///< requested burger type
  bool is_ready;                                            ///< true if burger is ready
  pthread_cond_t cond;                                      ///< conditional variable
  pthread_mutex_t mutex;                                    ///< mutex variable
} Node;

/// @brief order data
typedef struct __order_list {
  Node *head;                                               ///< head of order list
  Node *tail;                                               ///< tail of order list
  unsigned int count;                                       ///< number of nodes in list
} OrderList;

/// @brief structure for server context
struct mcdonalds_ctx {
  unsigned int total_customers;                             ///< number of customers served
  unsigned int total_burgers[BURGER_TYPE_MAX];              ///< number of burgers produced by types
  unsigned int total_queueing;                              ///< number of customers in queue
  OrderList list;                                           ///< starting point of list structure
};

/// @}

/// @name Global variables
/// @{

int listenfd;                                               ///< listen file descriptor
struct mcdonalds_ctx server_ctx;                            ///< keeps server context
sig_atomic_t keep_running = 1;                              ///< keeps all the threads running
pthread_t kitchen_thread[NUM_KITCHEN];                      ///< thread for kitchen
pthread_mutex_t server_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;

/// @}


/// @brief Enqueue element in tail of the OrderList
/// @param customerID customer ID
/// @param type burger type
/// @retval Node* containing the node structure of the element
Node* issue_order(unsigned int customerID, enum burger_type type)
{
  Node *new_node = malloc(sizeof(Node));

  new_node->customerID = customerID;
  new_node->type = type;
  new_node->next = NULL;
  new_node->is_ready = false;
  pthread_cond_init(&new_node->cond, NULL);
  pthread_mutex_init(&new_node->mutex, NULL);

  if (server_ctx.list.tail == NULL) {
    server_ctx.list.head = new_node;
    server_ctx.list.tail = new_node;
  } else {
    server_ctx.list.tail->next = new_node;
    server_ctx.list.tail = new_node;
  }

  server_ctx.list.count++;

  return new_node;
}

/// @brief Dequeue element from the OrderList
/// @retval Node* Node from head of the list
Node* get_order(void)
{
  Node *target_node;

  if (server_ctx.list.head == NULL) return NULL;

  target_node = server_ctx.list.head;


  if (server_ctx.list.head == server_ctx.list.tail) {
    server_ctx.list.head = NULL;
    server_ctx.list.tail = NULL;
  } else {
    server_ctx.list.head = server_ctx.list.head->next;
  }

  server_ctx.list.count--;


  return target_node;
}

/// @brief Returns number of element left in OrderList
/// @retval number of element(s) in OrderList
unsigned int order_left(void)
{
  int ret;

  ret = server_ctx.list.count;

  return ret;
}

/// @brief Kitchen task for kitchen thread
void* kitchen_task(void *dummy)
{
  Node *order;
  enum burger_type type;
  pthread_t tid = pthread_self();

  printf("Kitchen thread %lu ready\n", tid);

  while (keep_running || order_left()) {
    order = get_order();
    if (order == NULL) {
      sleep(2);
      continue;
    }

    type = order->type;
    printf("[Thread %lu] generating %s burger\n", tid, burger_names[type]);
    sleep(5);
    printf("[Thread %lu] %s burger is ready\n", tid, burger_names[type]);

    server_ctx.total_burgers[type]++;

    order->is_ready = true;
    pthread_cond_signal(&order->cond);
  }

  printf("[Thread %lu] terminated\n", tid);
  pthread_exit(NULL);
}

/// @brief error function for the serve_client
/// @param clientfd file descriptor of the client*
/// @param newsock socketid of the client as void*
/// @param newsock buffer for the messages*
void error_client(int clientfd, void *newsock,char *buffer) {
  close(clientfd);
  free(newsock);
  free(buffer);

  server_ctx.total_queueing--;
}

/// @brief client task for client thread
/// @param newsock socketID of the client as void*
void* serve_client(void *newsock)
{
  ssize_t read, sent;
  size_t msglen;
  char *message, *burger, *buffer;
  unsigned int customerID;
  enum burger_type type;
  Node *order = NULL;
  int ret, i, clientfd;

  clientfd = *(int *) newsock;
  buffer = (char *) malloc(BUF_SIZE);
  msglen = BUF_SIZE;

  customerID = server_ctx.total_customers++;

  printf("Customer #%d visited\n", customerID);

  // send welcome to mcdonalds
  ret = asprintf(&message, "Welcome to McDonald's, customer #%d\n", customerID);
  if (ret < 0) {
    perror("asprintf");
    return NULL;
  }

  sent = put_line(clientfd, message, ret);
  if (sent < 0) {
    printf("Error: cannot send data to client\n");
    error_client(clientfd, newsock, buffer);
    return NULL;
  }

  // Receive order from the customer
  read = get_line(clientfd, &message, &msglen);
  if (read <= 0) {
    printf("Error: cannot read data from client\n");
    error_client(clientfd, newsock, buffer);
    return NULL;
  }

  // Parse order from the customer
  burger = strtok(message, "\n");
  if (burger == NULL) {
    printf("Error: invalid order\n");
    error_client(clientfd, newsock, buffer);
    return NULL;
  }
  
  // Find burger type
  for (i = 0; i < BURGER_TYPE_MAX; i++) {
    if (strcmp(burger, burger_names[i]) == 0) {
      type = i;
      break;
    }
  }

  // If burger is not available, exit connection
  if (i == BURGER_TYPE_MAX) {
    printf("Error: burger not available\n");
    error_client(clientfd, newsock, buffer);
    return NULL;
  }

  // Issue order to kitchen and wait
  order = issue_order(customerID, type);
  // Wait until burger is available
  pthread_mutex_lock(&order->mutex);
  while (!order->is_ready) {
    pthread_cond_wait(&order->cond, &order->mutex);
  }
  pthread_mutex_unlock(&order->mutex);
  
  // if order successfully handled, hand burger and say goodbye
  if (order->is_ready) {
    ret = asprintf(&message, "Your %s burger is ready! Goodbye!\n", burger_names[i]);
    sent = put_line(clientfd, message, ret);
    if (sent <= 0) {
      printf("Error: cannot send data to client\n");
      error_client(clientfd, newsock, buffer);
      return NULL;
    }
    free(message);
  }
  
  free(order);

  close(clientfd);
  free(newsock);
  free(buffer);
  
  // Decrease number of total queueing
  pthread_mutex_lock(&server_ctx_mutex);
  server_ctx.total_queueing--;
  pthread_mutex_unlock(&server_ctx_mutex);

  return NULL;
}

/// @brief start server listening
void start_server()
{
  int clientfd, addrlen, opt = 1;
  struct sockaddr_in client;
  struct addrinfo *ai, *ai_it;

  // Get socket list
  int r;
  ai = getsocklist(IP, PORT, AF_UNSPEC, SOCK_STREAM, 1, &r);
  // If address info is null, print error message and exit
  if (ai == NULL) {
      fprintf(stderr, "getsocklist() failed: %s\n", gai_strerror(r));
      exit(EXIT_FAILURE);
  }

  // Travel address info iterator, try making socket and binding
  ai_it = ai;
  while (ai_it != NULL) {
    // Try making socket using address info
    listenfd = socket(ai_it->ai_family, ai_it->ai_socktype, ai_it->ai_protocol);
    // If making socket success, try setsockopt and binding
    if (listenfd != -1) {
      // If setsockopt failed, continue
      if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(listenfd);
        continue;
      }
      // If binding failed, continue
      if (bind(listenfd, ai_it->ai_addr, ai_it->ai_addrlen) == 0) {
        break;
      }
      // Close listenfd
      close(listenfd);
    }
    // Set ai_it to next address info
    ai_it = ai_it->ai_next; 
  }

  // Free address info
  freeaddrinfo(ai);

  // If binding is failed, print error message and exit
  if (ai_it == NULL) {
    fprintf(stderr, "Could not bind\n");
    exit(EXIT_FAILURE);
  }

  printf("Listening...\n");

  // Keep listening and accepting clients
  if (listen(listenfd, CUSTOMER_MAX) < 0) {
    close(listenfd);
    exit(EXIT_FAILURE);
  }

  // Loop to accept client connections. In this case, it will exit after handling one client.
  while (keep_running) {
    addrlen = sizeof(client);
    clientfd = accept(listenfd, (struct sockaddr *)&client, (socklen_t *)&addrlen);

    // If accepting a client connection fails
    if (clientfd < 0) {
      perror("accept");
      continue; // If accept fails, try again
    }

    // Lock server ctx mutex to avoid collisons
    pthread_mutex_lock(&server_ctx_mutex);
    if (server_ctx.total_queueing < CUSTOMER_MAX) {
      // Unlock server ctx mutex 
      pthread_mutex_unlock(&server_ctx_mutex);
      // Dynamically allocate memory for the client socket descriptor
      int *client_sock = malloc(sizeof(int));
      // If allocating client sock fails, perror
      if (client_sock == NULL) {
        perror("malloc");
        close(clientfd);
      } else { // If allocating client sock success, make client server thread
        *client_sock = clientfd;
        pthread_t thread_id;
        // Create thread for serve client, and success
        if (pthread_create(&thread_id, NULL, serve_client, client_sock) == 0) {
          // Detach pthread for concurrent work
          pthread_detach(thread_id);
          // Lock server ctx mutex to avoid collisons
          pthread_mutex_lock(&server_ctx_mutex);
          // Increase current queueing
          server_ctx.total_queueing++;
          // Unlock server ctx mutex 
          pthread_mutex_unlock(&server_ctx_mutex);
        } else { // If creating thread fails, perror and close clientfd and free client socket
          perror("pthread_create");
          close(clientfd);
          free(client_sock);
        }
      }
    } else {
      // Unlock server ctx mutex 
      pthread_mutex_unlock(&server_ctx_mutex);
      printf("Max number of customers exceeded, Good bye!\n");
      close(clientfd);
    }
    
  }
  

}

/// @brief prints overall statistics
void print_statistics(void)
{
  int i;

  printf("\n====== Statistics ======\n");
  printf("Number of customers visited: %u\n", server_ctx.total_customers);
  for (i = 0; i < BURGER_TYPE_MAX; i++) {
    printf("Number of %s burger made: %u\n", burger_names[i], server_ctx.total_burgers[i]);
  }
  printf("\n");
}

/// @brief exit function
void exit_mcdonalds(void)
{
  close(listenfd);
  print_statistics();
}

/// @brief Second SIGINT handler function
/// @param sig signal number
void sigint_handler2(int sig)
{
  exit_mcdonalds();
  exit(EXIT_SUCCESS);
}

/// @brief First SIGINT handler function
/// @param sig signal number
void sigint_handler(int sig)
{
  signal(SIGINT, sigint_handler2);
  printf("****** I'm tired, closing McDonald's ******\n");
  keep_running = 0;
  sleep(3);
  exit(EXIT_SUCCESS);
}

/// @brief init function initializes necessary variables and sets SIGINT handler
void init_mcdonalds(void)
{
  int i;

  printf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
  printf("@@@@@@@@@@@@@@@@@(,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,(@@@@@@@@@@@@@@@@@\n");
  printf("@@@@@@@@@@@@@@@,,,,,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,,,,,@@@@@@@@@@@@@@@\n");
  printf("@@@@@@@@@@@@@,,,,,,,@@@@@@,,,,,,,@@@@@@@@@@@@@@(,,,,,,@@@@@@@,,,,,,,@@@@@@@@@@@@@\n");
  printf("@@@@@@@@@@@@,,,,,,@@@@@@@@@@,,,,,,,@@@@@@@@@@@,,,,,,,@@@@@@@@@*,,,,,,@@@@@@@@@@@@\n");
  printf("@@@@@@@@@@.,,,,,,@@@@@@@@@@@@,,,,,,,@@@@@@@@@,,,,,,,@@@@@@@@@@@@,,,,,,/@@@@@@@@@@\n");
  printf("@@@@@@@@@,,,,,,,,@@@@@@@@@@@@@,,,,,,,@@@@@@@,,,,,,,@@@@@@@@@@@@@,,,,,,,,@@@@@@@@@\n");
  printf("@@@@@@@@,,,,,,,,@@@@@@@@@@@@@@@,,,,,,,@@@@@,,,,,,,@@@@@@@@@@@@@@@,,,,,,,,@@@@@@@@\n");
  printf("@@@@@@@@,,,,,,,@@@@@@@@@@@@@@@@,,,,,,,,@@@,,,,,,,,@@@@@@@@@@@@@@@@,,,,,,,@@@@@@@@\n");
  printf("@@@@@@@,,,,,,,,@@@@@@@@@@@@@@@@@,,,,,,,,@,,,,,,,,@@@@@@@@@@@@@@@@@,,,,,,,,@@@@@@@\n");
  printf("@@@@@@,,,,,,,,@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@,,,,,,,,@@@@@@\n");
  printf("@@@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@@@\n");
  printf("@@@@@,,,,,,,,@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@,,,,,,,,@@@@@\n");
  printf("@@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@@\n");
  printf("@@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@@\n");
  printf("@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@\n");
  printf("@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@\n");
  printf("@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@\n");
  printf("@@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@@\n");
  printf("@@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@@\n");
  printf("@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");

  printf("\n\n                          I'm lovin it! McDonald's\n\n");

  signal(SIGINT, sigint_handler);

  server_ctx.total_customers = 0;
  server_ctx.total_queueing = 0;
  for (i = 0; i < BURGER_TYPE_MAX; i++) {
    server_ctx.total_burgers[i] = 0;
  }
  // Make 5 kitchen threads
  for (i = 0; i < NUM_KITCHEN; i++) {
    pthread_create(&kitchen_thread[i], NULL, kitchen_task, NULL);
    pthread_detach(kitchen_thread[i]);
  }
}

/// @brief program entry point
int main(int argc, char *argv[])
{
  init_mcdonalds();
  start_server();
  exit_mcdonalds();

  return 0;
}
