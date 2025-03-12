#include "msg_que.h"
#include <stdlib.h>
#ifdef _WIN32
#include <Windows.h>
#define pthread_mutex_t CRITICAL_SECTION
#define pthread_mutex_init(x,y) InitializeCriticalSection(x)
#define pthread_mutex_lock EnterCriticalSection
#define pthread_mutex_unlock LeaveCriticalSection
#define pthread_mutex_destroy DeleteCriticalSection
#else
#include <pthread.h>
#endif // _WIN32

static pthread_mutex_t g_mutex;

void mq_init(struct MsgQue* q)
{
    pthread_mutex_init(&g_mutex, NULL);
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
    pthread_mutex_destroy(&g_mutex);
}

int mq_push(struct MsgQue* q, void* x)
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

void mq_pop(struct MsgQue* q)
{
    pthread_mutex_lock(&g_mutex);
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
    pthread_mutex_unlock(&g_mutex);
}

void* mq_front(struct MsgQue* q)
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

int mq_size(struct MsgQue* q)
{
    pthread_mutex_lock(&g_mutex);
    if (q != NULL) {
        INode* i = q->head;
        int s = 0;
        while (i != NULL) {
            i = i->next;
            s++;
        }
        pthread_mutex_unlock(&g_mutex);
        return s;
    }
    pthread_mutex_unlock(&g_mutex);
    return 0;
}
