#include "msg_que.h"
#include <stdlib.h>
#include <pthread.h>

static pthread_mutex_t g_mutex;

void queue_init(struct MsgQue* q)
{
    q->head = q->tail = NULL;
    pthread_mutex_init(&g_mutex, NULL);
}

void queue_del(struct MsgQue* q)
{
    pthread_mutex_destroy(&g_mutex);
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
    pthread_mutex_lock(&g_mutex);
    if (q == NULL) {
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }
    INode* i = (INode*)malloc(sizeof(INode));
    if (NULL == i) {
        pthread_mutex_unlock(&g_mutex);
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
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

void queue_pop(struct MsgQue* q)
{
    pthread_mutex_lock(&g_mutex);
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
    pthread_mutex_unlock(&g_mutex);
}

void* queue_front(struct MsgQue* q)
{
    pthread_mutex_lock(&g_mutex);
    if (q == NULL || q->head == NULL) {
        pthread_mutex_unlock(&g_mutex);
        return NULL;
    }
    void* p = q->head->data;
    pthread_mutex_unlock(&g_mutex);
    return p;
}

int queue_size(struct MsgQue* q)
{
    pthread_mutex_lock(&g_mutex);
    if (q != NULL) {
        INode* i = q->head;
        int s = 0;
        while (i != NULL) {
            i = i->next;
            s++;
        }
        pthread_mutex_lock(&g_mutex);
        return s;
    }
    pthread_mutex_lock(&g_mutex);
    return 0;
}
