#ifndef MSG_QUE_H
#define MSG_QUE_H

typedef struct iNode {
    void* data;
    struct iNode* next;
} INode;

typedef struct MsgQue {
    INode* head;
    INode* tail;
} MsgQue;

void queue_init(struct MsgQue* q);
int queue_push(struct MsgQue* q, void* x);
void* queue_front(struct MsgQue* q);
void queue_pop(struct MsgQue* q);
int queue_size(struct MsgQue* q);
void queue_deinit(struct MsgQue* q);

#endif
