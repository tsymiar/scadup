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

void mq_init(struct MsgQue* q);
int mq_push(struct MsgQue* q, void* x);
void* mq_front(struct MsgQue* q);
void mq_pop(struct MsgQue* q);
int mq_size(struct MsgQue* q);
void mq_deinit(struct MsgQue* q);

#endif
