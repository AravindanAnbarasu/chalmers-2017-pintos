/* Tests cetegorical mutual exclusion with different numbers of threads.
 * Automatic checks only catch severe problems like crashes.
 */
#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "lib/random.h" //generate random numbers

#define BUS_CAPACITY 3
#define SENDER 0
#define RECEIVER 1
#define NORMAL 0
#define HIGH 1


#define DEBUG false

/*
 *  initialize task with direction and priority
 *  call o
 * */
typedef struct {
    int direction;
    int priority;
} task_t;

//try to get a bandwidth
struct semaphore available_bandwidth;

size_t reader_ctr = 0; /* number of readers currently executing */
size_t writer_ctr = 0; /* number of writers currently executing */

/* number of waiting writers with high priority*/
size_t waiting_priority_writer_ctr = 0; 
/*number of waiting readers with high priority*/
size_t waiting_priority_reader_ctr = 0;

struct lock lock; /* Monitor lock. */
//condition variables for high priority write
//woken up from sleeping before threads waiting on ok_to_write
struct condition ok_to_write_priority;
//condition variables for high priority read (woken up before ok_to_read)
struct condition ok_to_read_priority;
//condition variables for normal priority write
//receive signal after ok_to_write_priority
struct condition ok_to_write;
//condition variables for normal priority read threads
//receive wakeup signal (if sleeping) after ok_to_read_priority
struct condition ok_to_read;


void batchScheduler(unsigned int num_tasks_send, unsigned int num_task_receive,
        unsigned int num_priority_send, unsigned int num_priority_receive);

void senderTask(void *);
void receiverTask(void *);
void senderPriorityTask(void *);
void receiverPriorityTask(void *);


void oneTask(task_t task); /*Task requires to use the bus and executes methods below*/
void getSlot(task_t task); /* task tries to use slot on the bus */
void transferData(task_t task); /* task processes data on the bus either sending or receiving based on the direction*/
void leaveSlot(task_t task); /* task release the slot */

/* initializes semaphores */
void init_bus(void) {
    //initialize a random number generator
    random_init((unsigned int) 123456789);

    //initialize the monitor lock
    lock_init(&lock);

    //initialize the condition variables
    cond_init(&ok_to_write);
    cond_init(&ok_to_read);
    cond_init(&ok_to_write_priority);
    cond_init(&ok_to_read_priority);
}

/*
 *  Creates a memory bus sub-system  with num_tasks_send + num_priority_send
 *  sending data to the accelerator and num_task_receive + num_priority_receive tasks
 *  reading data/results from the accelerator.
 *
 *  Every task is represented by its own thread. 
 *  Task requires and gets slot on bus system (1)
 *  process data and the bus (2)
 *  Leave the bus (3).
 */

void batchScheduler(unsigned int num_tasks_send, unsigned int num_task_receive,
        unsigned int num_priority_send, unsigned int num_priority_receive) {


    //make sure the user provides valid priority levels
    if (num_priority_send > PRI_MAX || num_priority_send < PRI_MIN) {
        if (DEBUG) msg("invalid num_priority_send");
        return;
    }
    if (num_priority_receive > PRI_MAX | num_priority_receive < PRI_MIN) {
        if (DEBUG) msg("invalid num_priority_send");
        return;
    }

    //create the sending threads with priority num_priority_send
    int i;

    //create the sending threads with high priority
    for (i = 0; i < num_priority_send; i++) {
        //name the treads
        char thread_name[100];
        snprintf(thread_name, sizeof thread_name,
                "Sending thread %i with priority %i",
                i,
                num_priority_send);
        //create the threads
        thread_create(thread_name, PRI_MAX, //makes sense only if pintos had a priority scheduler
                senderPriorityTask, NULL);
    }

    //create the receiving threads with night priorities
    for (i = 0; i < num_priority_receive; i++) {
        //name the threads
        char thread_name[100];
        snprintf(thread_name, sizeof thread_name,
                "Receiving thread %i with priority %i", i,
                num_priority_receive);
        //create the threads
        thread_create(thread_name, PRI_MAX, //makes sense only if pintos had a priority scheduler
                receiverPriorityTask, NULL);
    }

    //create a number of send threads with normal priority
    for (i = 0; i < num_tasks_send; i++) {
        //name the treads
        char thread_name[100];
        snprintf(thread_name, sizeof thread_name,
                "Sending thread %i with priority %i",
                i,
                num_priority_send);
        //create the threads
        thread_create(thread_name, PRI_DEFAULT, senderTask, NULL);
    }

    //create a number of receiving threads with normal priority
    for (i = 0; i < num_task_receive; i++) {
        //name the threads
        char thread_name[100];
        snprintf(thread_name, sizeof thread_name,
                "Receiving thread %i with priority %i", i,
                num_priority_receive);
        //create the threads
        thread_create(thread_name, PRI_DEFAULT,
                receiverTask, NULL);
    }

    //wait for threads we spawned to finish execution
    timer_sleep((int64_t) 10 * 100);
}

/* Normal task,  sending data to the accelerator */
void senderTask(void *aux UNUSED) {
    task_t task = {SENDER, NORMAL};
    oneTask(task);
}

/* High priority task, sending data to the accelerator */
void senderPriorityTask(void *aux UNUSED) {
    task_t task = {SENDER, HIGH};
    oneTask(task);
}

/* Normal task, reading data from the accelerator */
void receiverTask(void *aux UNUSED) {
    task_t task = {RECEIVER, NORMAL};
    oneTask(task);
}

/* High priority task, reading data from the accelerator */
void receiverPriorityTask(void *aux UNUSED) {
    task_t task = {RECEIVER, HIGH};
    oneTask(task);
}

/* abstract task execution*/
void oneTask(task_t task) {
    getSlot(task);
    transferData(task);
    leaveSlot(task);
}

/* task tries to get slot on the bus subsystem */
void getSlot(task_t task) {

    //determine task type
    switch (task.direction) {
        case RECEIVER:

            //acquire a lock,
            lock_acquire(&lock);

            //if there are any writers or more than 3 readers, sleep
            while (writer_ctr > 0 || reader_ctr >= 3) {
                //wait on the appropriate queue
                if (task.priority == HIGH) {

                    //increment the number of priority readers waiting
                    waiting_priority_reader_ctr++;

                    //if the tasks priority is high wait on ok_to_read_priority
                    //threads sleeping on ok_to_read_priority will be woken up
                    //before those which are waiting on ok_to_read
                    cond_wait(&ok_to_read_priority, &lock);

                    //decrement the number of priority readers waiting 
                    //since this thread has stopped waiting now
                    waiting_priority_reader_ctr--;
                } else {
                    //if this is not a high priority task
                    //wait on a regular queue
                    //thread will be woken up after high priority threads
                    cond_wait(&ok_to_read, &lock);
                }
            }
            //increment the reader counter(if reader_ctr>= 3 readers will wait
            //and writers will wait if reader_ctr > 0)
            reader_ctr++;
            //done! release the lock and go do the actual reading
            lock_release(&lock);

            break;
        case SENDER:
            //acquire a lock,
            lock_acquire(&lock);

            //if there are any readers or more than 3 writers, sleep
            while (reader_ctr > 0 || writer_ctr >= 3) {
                //put task in appropriate waiting queue
                if (task.priority == HIGH) {

                    //increment the number of priority writers waiting
                    waiting_priority_writer_ctr++;

                    //if the task priority is high, wait on ok_to_write_priority.
                    //Threads sleeping on ok_to_write_priority will be woken up
                    //before those which are waiting on ok_to_write
                    //when it is ok to write (no readers or < 3 writers)
                    cond_wait(&ok_to_write_priority, &lock);

                    //decrement the number of priority writers waiting 
                    //since this thread has stopped waiting now
                    waiting_priority_writer_ctr--;
                } else {
                    //if this is not a high priority task
                    //wait on a regular queue, woken up after high priority
                    //threads are done
                    cond_wait(&ok_to_write, &lock);
                }
            }

            //increment active writer count
            writer_ctr++;
            
            //done! release the lock and go do the actual reading
            lock_release(&lock);
            break;
    }

}

/* task processes data on the bus send/receive */
void transferData(task_t task) {
    //print some debugging info before starting read/write task
    if (DEBUG) printf("Starting %s data. Task Priority = %s\n",
            (task.direction == RECEIVER ? "Receiving" : "Sending"),
            (task.priority == HIGH ? "High." : "Normal"));
    //sleep a random number of seconds between (0 - 5 seconds)
    //assuming 100 ticks/sec
    int64_t ticks = random_ulong() % 500;
    timer_sleep(ticks);
    //print debugging info finishing read/write task
    if (DEBUG) printf("Done %s data. Task Priority = %s\n",
            (task.direction == RECEIVER ? "Receiving" : "Sending"),
            (task.priority == HIGH ? "High." : "Normal"));
}

/* task releases the slot */
void leaveSlot(task_t task) {

    if (task.direction == RECEIVER) {
        //acquire the lock 
        lock_acquire(&lock);
        //decrement the variable other threads are waiting on, thread is done
        reader_ctr--;

        //if there are waiting readers with high priority, wake them up first
        if (waiting_priority_reader_ctr > 0) {
            //wake up readers with high priority, if there is one
            cond_signal(&ok_to_read_priority, &lock);
        } else {
            //tell a regular waiting reader (if there exists one) to take the 
            //slot since there are no priority readers
            cond_signal(&ok_to_read, &lock);
        }

        //if this is the last reader, wake up priority writers and 
        //regular writers (the number of waken up threads will not exceed three)
        //though.
        if (reader_ctr == 0) {

            //determine the number of priority writers we need to wake up
            //if the number of waiting priority threads is less than 3, we may need
            //to wake up regular writers too
            int counter = waiting_priority_writer_ctr > 3 ? 3 :
                    waiting_priority_writer_ctr;
            while (counter >= 0) {
                //signal the waiting priority thread
                cond_signal(&ok_to_write_priority, &lock);
                //count the thread assuming that it will fill the slot
                --counter;
            }

            //determine the number of regular writers to wakeup
            //regular writers will occupy the remaining slots from priority writers
            counter = waiting_priority_writer_ctr < 3 ?
                    3 - waiting_priority_writer_ctr :
                    0;
            //wake up threads to fill the remaining slots
            while (counter >= 0) {
                //signal any waiting writer threads
                cond_signal(&ok_to_write, &lock);
                //count this thread 
                --counter;
            }
        }


        //release the monitor lock
        lock_release(&lock);

    } else {

        lock_acquire(&lock);
        //decrement the variable other threads are waiting on, thread is done
        writer_ctr--;


        //if there are waiting writers with high priority, wake them up first
        if (waiting_priority_writer_ctr > 0) {
            //wake up writers with high priority, if there are any
            cond_signal(&ok_to_write_priority, &lock);
        } else {
            //if no priority writer exists, signal a regular writer
            cond_signal(&ok_to_write, &lock);
        }

        //if this is the final writer, wake up priority readers and 
        //regular readers(the number of waken up threads will not exceed three)
        //though.
        if (writer_ctr == 0) {

            //determine the number of priority writers we need to wake up
            //if the number of waiting priority threads is less than 3, 
            //we need to wake up writers to fill the available slots
            int counter = waiting_priority_reader_ctr > 3 ? 3 :
                    waiting_priority_reader_ctr;
            while (counter >= 0) {
                //wake up readers with high priority
                cond_signal(&ok_to_read_priority, &lock);
                //count the thread
                --counter;
            }


            //determine the number of regular readers to wakeup
            //regular readers will occupy the remaining slots from priority
            //readers
            counter = waiting_priority_reader_ctr < 3 ?
                    3 - waiting_priority_reader_ctr :
                    0;
            //wake up reader threads to fill remaining slots
            while (counter >= 0) {
                //signal any regular reader threads
                cond_signal(&ok_to_read, &lock);
                //count this thread 
                --counter;
            }
        }

        //release the monitor lock
        lock_release(&lock);
    }

}
