#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct q_node {
    void *data;
    size_t data_size;
    struct q_node *next;
} q_node;

typedef struct Queue {
    QueueNode *front;
    QueueNode *rear;
    int size;
} Queue;

Queue *initQueue() {
    Queue *q = (Queue *)malloc(sizeof(Queue));
    if (q == NULL) {
        return NULL;
    }

    q->front = NULL;
    q->rear = NULL;
    q->size = 0;

    return q;
}

int isEmpty(Queue *q) {
    return (q->front == NULL);
}

int enqueue(Queue *q, void *data, size_t data_size) {
    QueueNode *newNode = (QueueNode *)malloc(sizeof(QueueNode));

    if (newNode == NULL) {
        return 0;
    }

    newNode->data = malloc(data_size);

    if (newNode->data == NULL) {
        free(newNode);
        return 0;
    }

    memcpy(newNode->data, data, data_size);
    newNode->data_size = data_size;
    newNode->next = NULL;

    if (isEmpty(q)) {
        q->front = newNode;
        q->rear = newNode;
    } else {
        q->rear->next = newNode;
        q->rear = newNode;
    }

    q->size++;

    return 1;
}

int dequeue(Queue *q, void *out_buffer, size_t buffer_size) {
    if (isEmpty(q)) {
        return 0;
    }

    QueueNode *temp = q->front;
    if (buffer_size < temp->data_size) {
        return 0;
    }

    memcpy(out_buffer, temp->data, temp->data_size);
    q->front = temp->next;
    if (q->front == NULL) {
        q->rear = NULL;
    }

    free(temp->data);
    free(temp);
    q->size--;
    return 1;
}

/* Peek front element without removing */
void *peek(Queue *q) {
    if (isEmpty(q)) {
        return NULL;
    }

    return q->front->data;
}

/* Get queue size */
int queueSize(Queue *q) {
    return q->size;
}

/* Destroy queue */
void destroyQueue(Queue *q) {
    while (!isEmpty(q)) {
        QueueNode *temp = q->front;
        q->front = temp->next;
        free(temp->data);
        free(temp);
    }
    free(q);
}
