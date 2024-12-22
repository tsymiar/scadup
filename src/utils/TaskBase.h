#ifndef TASKBASE_H
#define TASKBASE_H

class TaskBase {
public:
    virtual ~TaskBase() = default;
    virtual void execute() = 0;
};

#endif // TASKBASE_H
