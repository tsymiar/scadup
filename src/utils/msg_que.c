#include "msg_que.h"
#include <stdlib.h>

#ifdef _WIN32
#define MUTEX_INIT(m)    InitializeCriticalSection(m)
#define MUTEX_LOCK(m)    EnterCriticalSection(m)
#define MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#define MUTEX_DESTROY(m) DeleteCriticalSection(m)
#else
#define MUTEX_INIT(m)    pthread_mutex_init(m, NULL)
#define MUTEX_LOCK(m)    pthread_mutex_lock(m)
#define MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#define MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#endif

void mq_init(struct MsgQue* q)
{
    MUTEX_INIT(&q->mutex);
    q->head = q->tail = NULL;
}

void mq_deinit(struct MsgQue* q)
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
    MUTEX_DESTROY(&q->mutex);
}

int mq_push(struct MsgQue* q, void* x)
{
    MUTEX_LOCK(&q->mutex);
    if (q == NULL) {
        MUTEX_UNLOCK(&q->mutex);
        return -1;
    }
    INode* i = (INode*)malloc(sizeof(INode));
    if (NULL == i) {
        MUTEX_UNLOCK(&q->mutex);
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
    MUTEX_UNLOCK(&q->mutex);
    return 0;
}

void mq_pop(struct MsgQue* q)
{
    MUTEX_LOCK(&q->mutex);
    if (q != NULL && q->head != NULL) {
        if (q->head->next == NULL) {
            free(q->head);
            q->head = q->tail = NULL;
        } else {
            INode* n = q->head->next;
            free(q->head);
            q->head = n;
        }
    }
    MUTEX_UNLOCK(&q->mutex);
}

void* mq_front(struct MsgQue* q)
{
    MUTEX_LOCK(&q->mutex);
    if (q == NULL || q->head == NULL) {
        MUTEX_UNLOCK(&q->mutex);
        return NULL;
    }
    void* p = q->head->data;
    MUTEX_UNLOCK(&q->mutex);
    return p;
}

int mq_size(struct MsgQue* q)
{
    MUTEX_LOCK(&q->mutex);
    if (q != NULL) {
        INode* i = q->head;
        int s = 0;
        while (i != NULL) {
            i = i->next;
            s++;
        }
        MUTEX_UNLOCK(&q->mutex);
        return s;
    }
    MUTEX_UNLOCK(&q->mutex);
    return 0;
}
