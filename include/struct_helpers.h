#include <stdio.h>
#include <stdlib.h>

#ifndef __STRUCT_HELPERS_H__
// Queue
typedef struct q_node {
    void *data;
    size_t data_size;
    struct q_node *next;
} q_node;

typedef struct Queue {
    q_node *front;
    q_node *rear;
    int size;
} Queue;

Queue *init_queue();
q_node *create_q_node(void *data, size_t data_size);
int isEmpty(Queue *q);
int enqueue(Queue *q, void *data, size_t data_size);
int dequeue(Queue *q, void *out_buffer, size_t buffer_size);
void *peek(Queue *q);
int queue_size(Queue *q);
void destroy_queue(Queue *q);

// Linked List
typedef struct list_node {
    void *data;
    size_t data_size;
    struct list_node *next;
} list_node;

typedef struct LinkedList {
    list_node *head;
    int size;
} LinkedList;

LinkedList *init_list();
list_node *create_list_node(void *data, size_t data_size);
int push_front(LinkedList *list, void *data, size_t data_size);
int push_back(LinkedList *list, void *data, size_t data_size);
int popFront(LinkedList *list, void *out_buffer, size_t buffer_size);
list_node *getNode(LinkedList *list, int index);
void destroy_list(LinkedList *list);

#endif

