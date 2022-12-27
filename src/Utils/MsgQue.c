#include "MsgQue.h"
#include <stdlib.h>

void queue_init(struct MsgQue* q)
{
    q->head = q->tail = NULL;
}

void queue_del(struct MsgQue* q)
{
    if (q == NULL)
        return;
    INode* n = q->head;
    while (n) {
        INode* i = n->next;
        free(n);
        n = i;
    }
    q->tail = q->head = NULL;
}

int queue_push(struct MsgQue* q, void* x)
{
    if (q == NULL) {
        return -1;
    }
    INode* i = (INode*)malloc(sizeof(INode));
    if (NULL == i) {
        return -2;
    }
    i->data = x;
    i->next = NULL;

    if (q->tail == NULL) {
        q->head = q->tail = i;
    } else {
        q->tail->next = i;
        q->tail = i;
    }
    return 0;
}

void queue_pop(struct MsgQue* q)
{
    if (q != NULL && q->head != NULL) {
        if (q->head->next == NULL) {
            free(q->head);
            q->head = q->tail = NULL;
        }
    } else {
        INode* n = q->head->next;
        free(q->head);
        q->head = n;
    }
}

void* queue_front(struct MsgQue* q)
{
    if (q == NULL || q->head == NULL)
        return NULL;
    return q->head->data;
}

int queue_size(struct MsgQue* q)
{
    if (q != NULL) {
        INode* i = q->head;
        int s = 0;
        while (i != NULL) {
            i = i->next;
            s++;
        }
        return s;
    }
    return 0;
}
