#include <stdio.h>
#include <stdlib.h>
#include "struct_helpers.h"

// Queue
Queue *init_queue() {
    Queue *q = (Queue *)malloc(sizeof(Queue));
    if (q == NULL) {
        return NULL;
    }

    q->front = NULL;
    q->rear = NULL;
    q->size = 0;
    return q;
}

q_node *create_q_node(void *data, size_t data_size) {
    q_node *node = malloc(sizeof(q_node));
    if (node == NULL) {
        return NULL;
    }

    node->data = malloc(data_size);
    if (node->data == NULL) {
        free(node);
        return NULL;
    }

    memcpy(node->data, data, data_size);
    node->data_size = data_size;
    node->next = NULL;
    return node;
}

int isEmpty(Queue *q) {
    return (q->front == NULL);
}

int enqueue(Queue *q, void *data, size_t data_size) {
    q_node *new_node = (q_node *)malloc(sizeof(q_node));
    if (new_node == NULL) {
        return 1;
    }

    new_node->data = malloc(data_size);
    if (new_node->data == NULL) {
        free(new_node);
        return 1;
    }
    
    memcpy(new_node->data, data, data_size);
    new_node->data_size = data_size;
    new_node->next = NULL;

    if (isEmpty(q)) {
        q->front = new_node;
        q->rear = new_node;
    }
    else {
        q->rear->next = new_node;
        q->rear = new_node;
    }
    q->size++;
    return 0;
}

int dequeue(Queue *q, void *out_buffer, size_t buffer_size) {
    if (isEmpty(q)) {
        return 1;
    }

    q_node *temp = q->front;
    if (buffer_size < temp->data_size) {
        return 1;
    }

    memcpy(out_buffer, temp->data, temp->data_size);
    q->front = temp->next;
    if (q->front == NULL) {
        q->rear = NULL;
    }

    free(temp->data);
    free(temp);
    q->size--;
    return 0;
}

void *peek(Queue *q) {
    if (isEmpty(q)) {
        return NULL;
    }

    return q->front->data;
}

int queue_size(Queue *q) {
    return q->size;
}

void destroy_queue(Queue *q) {
    while (!isEmpty(q)) {
        q_node *temp = q->front;
        q->front = temp->next;
        free(temp->data);
        free(temp);
    }
    free(q);
}

// Linked List
LinkedList *init_list() {
    LinkedList *list = (LinkedList *)malloc(sizeof(LinkedList));
    if (list == NULL) {
        return NULL;
    }
    list->head = NULL;
    list->size = 0;

    return list;
}

list_node *create_list_node(void *data, size_t data_size) {
    list_node *node = (list_node *)malloc(sizeof(list_node));
    if (node == NULL) {
        return NULL;
    }

    node->data = malloc(data_size);
    if (node->data == NULL) {
        free(node);
        return NULL;
    }

    memcpy(node->data, data, data_size);
    node->data_size = data_size;
    node->next = NULL;
    return node;
}

int push_front(LinkedList *list, void *data, size_t data_size) {
    list_node *node = createNode(data, data_size);
    if (node == NULL) {
        return 1;
    }

    node->next = list->head;
    list->head = node;
    list->size++;
    return 0;
}

int push_back(LinkedList *list, void *data, size_t data_size) {
    list_node *node = createNode(data, data_size);
    if (node == NULL) {
        return 1;
    }

    if (list->head == NULL) {
        list->head = node;
    } 
    else {
        list_node *curr = list->head;
        while (curr->next != NULL) {
            curr = curr->next;
        }
        curr->next = node;
    }

    list->size++;
    return 0;
}

int popFront(LinkedList *list, void *out_buffer, size_t buffer_size) {
    if (list->head == NULL) {
        return 1;
    }

    list_node *temp = list->head;
    if (buffer_size < temp->data_size) {
        return 1;
    }

    memcpy(out_buffer, temp->data, temp->data_size);
    list->head = temp->next;
    free(temp->data);
    free(temp);
    list->size--;
    return 0;
}

list_node *getNode(LinkedList *list, int index) {
    if (index < 0 || index >= list->size) {
        return NULL;
    }
    list_node *curr = list->head;
    for (int i = 0; i < index; i++) {
        curr = curr->next;
    }

    return curr;
}

void destroy_list(LinkedList *list) {
    list_node *curr = list->head;
    while (curr != NULL) {
        list_node *temp = curr;
        curr = curr->next;
        free(temp->data);
        free(temp);
    }
    free(list);
}
