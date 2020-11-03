/*	Name:		Epharra Mendoza
	DUE:		11/2/2020
	COURSE:		CMPSCI 4760-001
	ASSIGNMENT:	4
	FILENAME:	oss.c
	DESCRIPTION:	Selects a process to run and schedule it for execution using scheduling algorithm.*/

#include "shared.h"


/*==============================================================================*/
/*==============================================================================*/
/*
			GLOBALS AND PROTOTYPES
*/
/*==============================================================================*/
/*==============================================================================*/

static FILE *fpw = NULL;				//File pointer
static char *g_exe_name;				//Holds the name of first argument(program name)	

//Memory globals
static int g_mqueueid = -1;				//Holds message queue identifier	
struct Message g_master_message;			//Message struct (shared.h). Holds message details
static struct Queue *highQueue;				//Queue struct (shared.h). Holds high priorities
static struct Queue *midQueue;				//Queue struct (shared.h). Between high and low
static struct Queue *lowQueue;				//Queue struct (shared.h). Holds low priorities
static int g_shmclock_shmid = -1;			//Shared memory segment id for shared  clock
static struct SharedClock *g_shmclock_shmptr = NULL;	//Pointer to SharedClock struct (shared.h). 
static int g_semid = -1;				//Global shared memory segment id
static struct sembuf g_sema_operation;			//Struct with members for semaphore operations
static int g_pcbt_shmid = -1;				//Shared memory segment id for process control block
static struct ProcessControlBlock *g_pcbt_shmptr = NULL;	//Pointer to Process Control Block Struct (shared.h)
static key_t g_key;					//Integer of type key_t for shared memory

//Fork globals
static int g_fork_number = 0;				//Holds total number of forks in execution
static pid_t g_pid = -1;				//Process id type pid_t
static unsigned char g_bitmap[MAX_PROCESS];		//Bit array length 18

//Prototypes
void masterInterrupt(int seconds);			//Listens for keypress or time exceeded
void masterHandler(int signum);				//After interrupt, prints final info and closes log
void exitHandler(int signum);				//Exit program
void timer(int seconds);				//Timer function
void finalize();					//Sends signal to all children and invokes termination
void discardShm(int shmid, void *shmaddr, char *shm_name , char *exe_name, char *process_type);	//Detach shared memory
void cleanUp();						//Release shared memory
void semaLock(int sem_index);				//Semaphore lock on given semaphore 
void semaRelease(int sem_index);			//Release semaphore lock
void incShmclock();					//Increment clock
struct Queue *createQueue();				//Create queue
struct QNode *newNode(int index);			//New node for queue
void enQueue(struct Queue* q, int index);		//Push to queue
struct QNode *deQueue(struct Queue *q);			//Remove from queue
bool isQueueEmpty(struct Queue *q);			//Check if queue is empty
struct UserProcess *initUserProcess(int index, pid_t pid);	//Struct with process details (shared.h)
void userToPCB(struct ProcessControlBlock *pcb, struct UserProcess *user);	//Transfer user process info to process control block






/*==============================================================================*/
/*==============================================================================*/
/*
				MAIN
*/
/*==============================================================================*/
/*==============================================================================*/


int main(int argc, char *argv[]) 
{
	//Read name of program
	g_exe_name = argv[0];
	//Seed random
	srand(getpid());

	
	/*-------------------Capturing User Input-----------------------------*/
	//Log file name
	char log_file[256] = "log.dat";
	
	//User input option char
	int opt;
	//Use getopt function to parse arguments
	while((opt = getopt(argc, argv, "hl:")) != -1)
	{
		switch(opt)
		{
			//Help option
			case 'h':
				printf("NAME:\n");
				printf("	%s - simulate the process scheduling part of an operating system with the use of message queues for synchronization.\n", g_exe_name);
				printf("\nUSAGE:\n");
				printf("	%s [-h] [-l logfile] [-t time].\n", g_exe_name);
				printf("\nDESCRIPTION:\n");
				printf("	-h          : print the help page and exit.\n");
				printf("	-l filename : the log file used (default log.dat).\n");
				exit(0);

			case 'l':
				strncpy(log_file, optarg, 255);
				printf("Your new log file is: %s\n", log_file);
				break;
				
			default:
				fprintf(stderr, "%s: please use \"-h\" option for more info.\n", g_exe_name);
				exit(EXIT_FAILURE);
		}
	}
	
	//Check for extra arguments. Return with error message if too many arguments
	if(optind < argc)
	{
		fprintf(stderr, "%s ERROR: extra arguments was given! Please use \"-h\" option for more info.\n", g_exe_name);
		exit(EXIT_FAILURE);
	}


	/*----------------------Open log file for write----------------------------*/
	//Open file
	fpw = fopen(log_file, "w");
	//If there is an issue opening file, return with error message
	if(fpw == NULL)
	{
		fprintf(stderr, "%s ERROR: unable to write the output file.\n", argv[0]);
		exit(EXIT_FAILURE);
	}


	
	/*---------------------Initialize Bitmap---------------------------------*/
	//Zero out all elements of bit map
	memset(g_bitmap, '\0', sizeof(g_bitmap));


	
	/*---------------------Initialize Message Queue--------------------------*/
	//Allocate shared memory if doesn't exist, and check if it can create one. Return ID for [queue] shared memory

	//Use ftok to convert pathname and id value '1' to System V IPC key
	g_key = ftok("./oss.c", 1);
	g_mqueueid = msgget(g_key, IPC_CREAT | 0600);
	//Return with error message if issue creating shared memory
	if(g_mqueueid < 0)
	{
		fprintf(stderr, "%s ERROR: could not allocate [queue] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}


	/*---------------------Initialize Shared Memory Clock---------------------*/
	//Allocate shared memory if doesn't exist, and check if can create one. Return ID for [shmclock] shared memory
	
	//Use ftok to convert pathname to id value '2' to System V IPC key
	g_key = ftok("./oss.c", 2);
	g_shmclock_shmid = shmget(g_key, sizeof(struct SharedClock), IPC_CREAT | 0600);
	//Return with error message if issue creating shared memory
	if(g_shmclock_shmid < 0)
	{
		fprintf(stderr, "%s ERROR: could not allocate [shmclock] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	//Attaching shared memory and check if can attach it. If not, delete the [shmclock] shared memory
	g_shmclock_shmptr = shmat(g_shmclock_shmid, NULL, 0);
	if(g_shmclock_shmptr == (void *)( -1 ))
	{
		fprintf(stderr, "%s ERROR: fail to attach [shmclock] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);	
	}

	//Initialize shared memory attribute of [shmclock] to 0
	g_shmclock_shmptr->second = 0;
	g_shmclock_shmptr->nanosecond = 0;


	/*---------------------Initialize Semaphore---------------------*/
	//Creating 3 semaphores elements
	//Create semaphore if doesn't exist with 666 bits permission. Return error if semaphore already exists
	
	//Use ftok to convert pathname to id value '3' to System V IPC key
	g_key = ftok("./oss.c", 3);
	g_semid = semget(g_key, 3, IPC_CREAT | IPC_EXCL | 0600);
	//Return with error message if issue creating semaphore
	if(g_semid == -1)
	{
		fprintf(stderr, "%s ERROR: failed to create a new private semaphore! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}
	
	//Initialize the semaphore(s) in our set to 1
	semctl(g_semid, 0, SETVAL, 1);	//Semaphore #0: for [shmclock] shared memory
	semctl(g_semid, 1, SETVAL, 1);	//Semaphore #1: for terminating user process
	semctl(g_semid, 2, SETVAL, 1);	//Semaphore #2: for [pcbt] shared memory
	


	/*-----------Initialize Process Control Block Table--------------*/
	//Allocate shared memory if doesn't exist, and check if can create one. Return ID for [pcbt] shared memory
	
	//Use ftok to convert pathname to id vale '4' to System V IPC key
	g_key = ftok("./oss.c", 4);
	size_t process_table_size = sizeof(struct ProcessControlBlock) * MAX_PROCESS;
	g_pcbt_shmid = shmget(g_key, process_table_size, IPC_CREAT | 0600);
	//Return with error message if issue creating process control block
	if(g_pcbt_shmid < 0)
	{
		fprintf(stderr, "%s ERROR: could not allocate [pcbt] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	//Attaching shared memory and check if can attach it. If not, delete the [pcbt] shared memory
	g_pcbt_shmptr = shmat(g_pcbt_shmid, NULL, 0);
	if(g_pcbt_shmptr == (void *)( -1 ))
	{
		fprintf(stderr, "%s ERROR: fail to attach [pcbt] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);	
	}


	/*-----------Create high, mid, and low queues--------------*/
	//Set up the high, mid, and low priority queues
	highQueue = createQueue();
	midQueue = createQueue();
	lowQueue = createQueue();


	/*-Master Interrupt Signal Handling for 3 second timer-----*/
	masterInterrupt(3);


	/*-----------Fork processes and fill bitmap--------------*/
	int last_index = -1;	//bitmap position
	bool is_open = false;	//Is bitmap spot available
	int current_queue = 0;	//Current queue
	float mid_average_wait_time = 0.0;	//Wait time for mid queue
	float low_average_wait_time = 0.0;	//Wait time for low queue
	//Bit map filling
	while(1)
	{
		//Is this spot in the bit map open
		is_open = false;
		//Holds a count of processes
		int count_process = 0;
			
		while(1)
		{	
			//Bitmap position where position cannot exceed Max Processes
			last_index = (last_index + 1) % MAX_PROCESS;
			//Left shift. Algorithm source: geeksforgeeks.com
			uint32_t bit = g_bitmap[last_index / 8] & (1 << (last_index % 8));
			//If space is open
			if(bit == 0)
			{
				is_open = true;
				break;
			}
			//Else full
			else
			{
				is_open = false;
			}
			//Check that processes does not exceed max allowed processes
			if(count_process >= MAX_PROCESS - 1)
			{
				//Bitmap is full message
				fprintf(stderr, "%s: bitmap is full (size: %d)\n", g_exe_name, MAX_PROCESS);
				fprintf(fpw, "%s: bitmap is full (size: %d)\n", g_exe_name, MAX_PROCESS);
				//Flush output buffer of file stream
				fflush(fpw);
				break;
			}
			//Increase amount of processes by 1
			count_process++;
		}


		//Continue to fork if there are still space in the bitmap
		if(is_open == true)
		{
			//Fork a child
			g_pid = fork();
			//If error forking then return with error message
			if(g_pid == -1)
			{
				fprintf(stderr, "%s (Master) ERROR: %s\n", g_exe_name, strerror(errno));
				//Clean up memory then exit
				finalize();
				cleanUp();
				exit(0);
			}
			//If forking successful
			if(g_pid == 0) //Child
			{
				//Simple signal handler: this is for mis-synchronization when timer fire off
				signal(SIGUSR1, exitHandler);

				//Replaces the current running process with a new process (user)
				int exect_status = execl("./user_proc", "./user_proc", NULL);
				//If failed to execute, return with error message
				if(exect_status == -1)
        		{	
					fprintf(stderr, "%s (Child) ERROR: execl fail to execute! Exiting...\n", g_exe_name);
				}
				//Clean up memory then exit
				finalize();
				cleanUp();
				exit(EXIT_FAILURE);
			}
			else //Parent
			{	
				//Increment the total number of fork in execution
				g_fork_number++;
				//Set the current index to one bit (meaning it is taken)
				g_bitmap[last_index / 8] |= (1 << (last_index % 8));
				//Initialize the user process and transfer the user process information to the process control block table
				struct UserProcess *user = initUserProcess(last_index, g_pid);
				userToPCB(&g_pcbt_shmptr[last_index], user);

				//Add the process to highest queue
				enQueue(highQueue, last_index);

				//Display creation time
				fprintf(stderr, "%s: generating process with PID (%d) [%d] and putting it in queue [%d] at time %d.%d\n", 
					g_exe_name, g_pcbt_shmptr[last_index].pidIndex, g_pcbt_shmptr[last_index].actualPid, 
					g_pcbt_shmptr[last_index].priority, g_shmclock_shmptr->second, g_shmclock_shmptr->nanosecond);
						
				fprintf(fpw, "%s: generating process with PID (%d) [%d] and putting it in queue [%d] at time %d.%d\n", 
					g_exe_name, g_pcbt_shmptr[last_index].pidIndex, g_pcbt_shmptr[last_index].actualPid, 
					g_pcbt_shmptr[last_index].priority, g_shmclock_shmptr->second, g_shmclock_shmptr->nanosecond);
				//Flush output buffer
				fflush(fpw);
			}
		}


		/*-----------Scheduling Queue (Multilevel Queue)--------------*/
		//Print current queue working with
		fprintf(stderr, "\n%s: working with queue [%d]\n", g_exe_name, current_queue);
		fprintf(fpw, "\n%s: working with queue [%d]\n", g_exe_name, current_queue);
		//Flush output buffer
		fflush(fpw);
		//Next node
		struct QNode next;
		//Set front position of each queue
		//If high priority Queue
		if(current_queue == 0)
		{
			next.next = highQueue->front;
		}
		//If mid priority queue
		else if(current_queue == 1)
		{
			next.next = midQueue->front;
		}
		//If low priority queue
		else if(current_queue == 2)
		{
			next.next = lowQueue->front;
		}
		//Set total processes and wait time to 0
		int total_process = 0;
		float total_wait_time = 0.0;
		struct Queue *tempQueue = createQueue();
		//While next node is not empty
		while(next.next != NULL)
		{
			//Increase total processes by 1
			total_process++;

			//- CRITICAL SECTION -//
			//Increase shared memory clock
			incShmclock();

			//Sending a message to a specific child to tell him it is his turn
			//Child index is next node position index
			int c_index = next.next->index;
			//Set g_master_message attributes
			g_master_message.mtype = g_pcbt_shmptr[c_index].actualPid;
			g_master_message.index = c_index;
			g_master_message.childPid = g_pcbt_shmptr[c_index].actualPid;
			g_master_message.priority = g_pcbt_shmptr[c_index].priority = current_queue;
			//Send message
			msgsnd(g_mqueueid, &g_master_message, (sizeof(struct Message) - sizeof(long)), 0);
			
			//Print signaling process message to console
			fprintf(stderr, "%s: signaling process with PID (%d) [%d] from queue [%d] to dispatch\n",
				g_exe_name, g_master_message.index, g_master_message.childPid, current_queue);

			fprintf(fpw, "%s: signaling process with PID (%d) [%d] from queue [%d] to dispatch\n",
				g_exe_name, g_master_message.index, g_master_message.childPid, current_queue);
			//Flush output buffer
			fflush(fpw);

			//- CRITICAL SECTION -//
			//Increase shared memory clock
			incShmclock();

			//Getting dispatching time from the child
			msgrcv(g_mqueueid, &g_master_message, (sizeof(struct Message) - sizeof(long)), 1, 0);
			
			//Print dispatching process message to console
			fprintf(stderr, "%s: dispatching process with PID (%d) [%d] from queue [%d] at time %d.%d\n",
				g_exe_name, g_master_message.index, g_master_message.childPid, current_queue, 
				g_master_message.second, g_master_message.nanosecond);

			fprintf(fpw, "%s: dispatching process with PID (%d) [%d] from queue [%d] at time %d.%d\n",
				g_exe_name, g_master_message.index, g_master_message.childPid, current_queue, 
				g_master_message.second, g_master_message.nanosecond);
			//Flush output buffer
			fflush(fpw);

			//- CRITICAL SECTION -//
			//Increase shared memory clock
			incShmclock();

			//Calculate the total time of the dispatch
			int difference_nanosecond = g_shmclock_shmptr->nanosecond - g_master_message.nanosecond;
			
			//Print total time process message to console
			fprintf(stderr, "%s: total time of this dispatch was %d nanoseconds\n", g_exe_name, difference_nanosecond);

			fprintf(fpw, "%s: total time of this dispatch was %d nanoseconds\n", g_exe_name, difference_nanosecond);
			//Flush output buffer
			fflush(fpw);


			/*---------------Calculate how long current process is running--------------*/
			//Getting how long the current process is running (in nanosecond)
			while(1)
			{
				//- CRITICAL SECTION -//
				//Increase shared memory clock
				incShmclock();
				
				//Get time
				int result = msgrcv(g_mqueueid, &g_master_message, (sizeof(struct Message) - sizeof(long)), 1, IPC_NOWAIT);
				//If ran
				if(result != -1)
				{
					//Print running time process message to console
					fprintf(stderr, "%s: receiving that process with PID (%d) [%d] ran for %d nanoseconds\n", 
						g_exe_name, g_master_message.index, g_master_message.childPid, g_master_message.burstTime);

					fprintf(fpw, "%s: receiving that process with PID (%d) [%d] ran for %d nanoseconds\n", 
						g_exe_name, g_master_message.index, g_master_message.childPid, g_master_message.burstTime);
					//Flush output buffer
					fflush(fpw);
					break;
				}
			}

			//- CRITICAL SECTION -//
			//Increase shared memory clock
			incShmclock();
			
			
			/*------------------Check if child finished------------------------------*/
			//Getting if the child finish running or not. Update process control block base on the receive message
			msgrcv(g_mqueueid, &g_master_message, (sizeof(struct Message) - sizeof(long)), 1, 0);
			
			//Process control block attributes update with child index position
			g_pcbt_shmptr[c_index].lastBurst = g_master_message.burstTime;
			g_pcbt_shmptr[c_index].totalBurst += g_master_message.burstTime;
			g_pcbt_shmptr[c_index].totalSystemSecond = g_master_message.spawnSecond;
			g_pcbt_shmptr[c_index].totalSystemNanosecond = g_master_message.spawnNanosecond;
			g_pcbt_shmptr[c_index].totalWaitSecond += g_master_message.waitSecond;
			g_pcbt_shmptr[c_index].totalWaitNanosecond += g_master_message.waitNanosecond;
			
			//If no flags
			if(g_master_message.flag == 0)
			{	
				//Print process control attributes message to console
				fprintf(stderr, "%s: process with PID (%d) [%d] has finish running at my time %d.%d\n\tLast Burst (nano): %d\n\tTotal Burst (nano): %d\n\tTotal CPU Time (second.nano): %d.%d\n\tTotal Wait Time (second.nano): %d.%d\n",
					g_exe_name, g_master_message.index, g_master_message.childPid, g_shmclock_shmptr->second, g_shmclock_shmptr->nanosecond,
					g_pcbt_shmptr[c_index].lastBurst, g_pcbt_shmptr[c_index].totalBurst, g_pcbt_shmptr[c_index].totalSystemSecond, 
					g_pcbt_shmptr[c_index].totalSystemNanosecond, g_pcbt_shmptr[c_index].totalWaitSecond, g_pcbt_shmptr[c_index].totalWaitNanosecond);

				fprintf(fpw, "%s: process with PID (%d) [%d] has finish running at my time %d.%d\n\tLast Burst (nano): %d\n\tTotal Burst (nano): %d\n\tTotal CPU Time (second.nano): %d.%d\n\tTotal Wait Time (second.nano): %d.%d\n",
					g_exe_name, g_master_message.index, g_master_message.childPid, g_shmclock_shmptr->second, g_shmclock_shmptr->nanosecond,
					g_pcbt_shmptr[c_index].lastBurst, g_pcbt_shmptr[c_index].totalBurst, g_pcbt_shmptr[c_index].totalSystemSecond, 
					g_pcbt_shmptr[c_index].totalSystemNanosecond, g_pcbt_shmptr[c_index].totalWaitSecond, g_pcbt_shmptr[c_index].totalWaitNanosecond);
				//Flush output buffer
				fflush(fpw);
				
				//Update total wait time 
				total_wait_time += g_master_message.waitTime;
			}
			//Else if flag
			else
			{
				//If in high priority queue
				if(current_queue == 0)
				{
					//If used all quantum time
					//Move process to mid queue
					if(g_master_message.waitTime > (ALPHA * mid_average_wait_time))
					{
						//Print process relocation message to console
						fprintf(stderr, "%s: putting process with PID (%d) [%d] to queue [1]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);

						fprintf(fpw, "%s: putting process with PID (%d) [%d] to queue [1]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);
						//Flush output buffer
						fflush(fpw);
						
						//Push to midque
						enQueue(midQueue, c_index);
						//Update child priority to be mid
						g_pcbt_shmptr[c_index].priority = 1;
					}
					//Did not use entire quantum time so move to high priority
					else
					{
						//Print prcess relocation message to console
						fprintf(stderr, "%s: not using its entire time quantum. Putting process with PID (%d) [%d] to queue [0]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);

						fprintf(fpw, "%s: not using its entire time quantum. Putting process with PID (%d) [%d] to queue [0]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);
						//Flush output  buffer
						fflush(fpw);
						//Push to tempqueue
						enQueue(tempQueue, c_index);
						//Update child priority to be high
						g_pcbt_shmptr[c_index].priority = 0;
					}
				}
				//Else if current queue is mid
				else if(current_queue == 1)
				{
					//If used all quantum time
					//Move process to low queue
					if(g_master_message.waitTime > (BETA * low_average_wait_time))
					{
						//Print process relocation message to console
						fprintf(stderr, "%s: putting process with PID (%d) [%d] to queue [2]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);

						fprintf(fpw, "%s: putting process with PID (%d) [%d] to queue [2]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);
						//Flush output buffer
						fflush(fpw);
						//Push to low queue
						enQueue(lowQueue, c_index);
						//Update child priority to be low
						g_pcbt_shmptr[c_index].priority = 2;
					}
					//Did not use entire quantum time
					else
					{
						//Print process relocation messsage to console
						fprintf(stderr, "%s: not using its entire time quantum. Putting process with PID (%d) [%d] to queue [1]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);

						fprintf(fpw, "%s: not using its entire time quantum. Putting process with PID (%d) [%d] to queue [1]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);
						//Flush output buffer
						fflush(fpw);
						//Push to tempqueue
						enQueue(tempQueue, c_index);
						//Update child priority to be mid
						g_pcbt_shmptr[c_index].priority = 1;
					}
				}
				//Else if current queue is low
				else if(current_queue == 2)
				{
					//Print process location message to console
					fprintf(stderr, "%s: putting process with PID (%d) [%d] to queue [2]\n",
						g_exe_name, g_master_message.index, g_master_message.childPid);

					fprintf(fpw, "%s: putting process with PID (%d) [%d] to queue [2]\n",
						g_exe_name, g_master_message.index, g_master_message.childPid);
					//Flush output buffer
					fflush(fpw);
					//Push to tempqueue
					enQueue(tempQueue, c_index);
					//Child priority is low
					g_pcbt_shmptr[c_index].priority = 2;
				}
				//Total wait time update
				total_wait_time += g_master_message.waitTime;
			}

			
			/*-------------Update pointer in queue------------------------------*/
			//Point the pointer to the next queue element
			if(next.next->next != NULL)
			{
				next.next = next.next->next;
			}
			else
			{
				next.next = NULL;
			}
		}


		/*-------------------Calculate average wait times-------------------------*/
		//Calculate the average time for the next queue
		if(total_process == 0)
		{
			total_process = 1;
		}
		//Mid queue
		if(current_queue == 1)
		{
			mid_average_wait_time = (total_wait_time / total_process);
		}
		//Low queue
		else if(current_queue == 2)
		{
			low_average_wait_time = (total_wait_time / total_process);
		}


		/*---------------Reassign current queue----------------------------*/
		//Reassigned the current queue
		//If in high priority queue
		if(current_queue == 0)
		{
			//While not empty
			while(!isQueueEmpty(highQueue))
			{
				//Dequeue
				deQueue(highQueue);
			}
			//While temp queue not empty
			while(!isQueueEmpty(tempQueue))
			{
				//Go to front of tempqueue, grab index and enqueue into highQueue
				int i = tempQueue->front->index;
				enQueue(highQueue, i);
				//Dequeue tempQueue
				deQueue(tempQueue);
			}
		}
		//If in mid priority queue
		else if(current_queue == 1)
		{
			//While not empty
			while(!isQueueEmpty(midQueue))
			{
				//Dequeue
				deQueue(midQueue);
			}
			//While temp queue not empty
			while(!isQueueEmpty(tempQueue))
			{
				//Go to front of tempQueue, grab index and enqueue into midqueue
				int i = tempQueue->front->index;
				enQueue(midQueue, i);
				//Dequeue tempqueue
				deQueue(tempQueue);
			}
		}
		//Else if in low priority queue
		else if(current_queue == 2)
		{
			//While not empty
			while(!isQueueEmpty(lowQueue))
			{
				//Dequeue
				deQueue(lowQueue);
			}
			//While temp queue not empty
			while(!isQueueEmpty(tempQueue))
			{
				//Go to front of tempqueue, grab index and enqueue into lowqueue
				int i = tempQueue->front->index;
				enQueue(lowQueue, i);
				//Dequeue tempQueue
				deQueue(tempQueue);
			}
		}
		//Free tempqueue
		free(tempQueue);


		/*-------Incrememnt the next queue check---------------------------*/
		//Increment the next queue check
		current_queue = (current_queue + 1) % 3;

		//- CRITICAL SECTION -//
		//Increment shared memory clock
		incShmclock();

		/*--------Check child status------------------------------------*/
		//Check to see if a child exit, wait no bound (return immediately if no child has exit)
		int child_status = 0;
		pid_t child_pid = waitpid(-1, &child_status, WNOHANG);

		//Set the return index bit back to zero (which mean there is a spot open for this specific index in the bitmap)
		if(child_pid > 0)
		{
			int return_index = WEXITSTATUS(child_status);
			g_bitmap[return_index / 8] &= ~(1 << (return_index % 8));
		}

		/*-------Check fork number--------------------------------------*/
		//End the infinite loop when reached X forking times. Turn off timer to avoid collision.
		if(g_fork_number >= TOTAL_PROCESS)
		{
			timer(0);
			masterHandler(0);
		}
	}

	return EXIT_SUCCESS; 
}






/*==============================================================================*/
/*==============================================================================*/
/*
				FUNCTIONS
*/
/*==============================================================================*/
/*==============================================================================*/



/* Function    :  masterInterrupt(), masterHandler(), and exitHandler()
Definition  :  Interrupt master process base on given time or user interrupt via keypress. Send a 
                  terminate signal to all the child process and "user" process. */

void masterInterrupt(int seconds)
{
	timer(seconds);
	signal(SIGUSR1, SIG_IGN);

	struct sigaction sa1;
	sigemptyset(&sa1.sa_mask);
	sa1.sa_handler = &masterHandler;
	sa1.sa_flags = SA_RESTART;
	if(sigaction(SIGALRM, &sa1, NULL) == -1)
	{
		perror("ERROR");
	}

	struct sigaction sa2;
	sigemptyset(&sa2.sa_mask);
	sa2.sa_handler = &masterHandler;
	sa2.sa_flags = SA_RESTART;
	if(sigaction(SIGINT, &sa2, NULL) == -1)
	{
		perror("ERROR");
	}
}
void masterHandler(int signum)
{
	finalize();

	//Print out basic statistic
	fprintf(stderr, "- Master PID: %d\n", getpid());
	fprintf(stderr, "- Number of forking during this execution: %d\n", g_fork_number);
	fprintf(stderr, "- Final simulation time of this execution: %d.%d\n", g_shmclock_shmptr->second, g_shmclock_shmptr->nanosecond);

	double throughput = g_shmclock_shmptr->second + ((double)g_shmclock_shmptr->nanosecond / 10000000000.0);
	throughput = (double)g_fork_number / throughput;
	fprintf(stderr, "- Throughput Time: %f (process per time)\n", throughput);

	cleanUp();

	//Final check for closing log file
	if(fpw != NULL)
	{
		fclose(fpw);
		fpw = NULL;
	}

	exit(EXIT_SUCCESS); 
}
void exitHandler(int signum)
{
	printf("%d: Terminated!\n", getpid());
	exit(EXIT_SUCCESS);
}




/* Function    :  timer()
* Definition  :  Create a timer that decrement in real time. Once the timer end, send out SIGALRM.*/
void timer(int seconds)
{
	//Timers decrement from it_value to zero, generate a signal, and reset to it_interval.
	//A timer which is set to zero (it_value is zero or the timer expires and it_interval is zero) stops.
	struct itimerval value;
	value.it_value.tv_sec = seconds;
	value.it_value.tv_usec = 0;
	value.it_interval.tv_sec = 0;
	value.it_interval.tv_usec = 0;
	
	if(setitimer(ITIMER_REAL, &value, NULL) == -1)
	{
		perror("ERROR");
	}
}




/* Function    :  finalize()
* Definition  :  Send a SIGUSR1 signal to all the child process and "user" process.*/
void finalize()
{
	fprintf(stderr, "\nLimitation has reached! Invoking termination...\n");
	kill(0, SIGUSR1);
	pid_t p = 0;
	while(p >= 0)
	{
		p = waitpid(-1, NULL, WNOHANG);
	}
}





/* Function    :  discardShm()
* Definition  :  Detach and remove any shared memory.*/
void discardShm(int shmid, void *shmaddr, char *shm_name , char *exe_name, char *process_type)
{
	//Detaching...
	if(shmaddr != NULL)
	{
		if((shmdt(shmaddr)) << 0)
		{
			fprintf(stderr, "%s (%s) ERROR: could not detach [%s] shared memory!\n", exe_name, process_type, shm_name);
		}
	}
	
	//Deleting...
	if(shmid > 0)
	{
		if((shmctl(shmid, IPC_RMID, NULL)) < 0)
		{
			fprintf(stderr, "%s (%s) ERROR: could not delete [%s] shared memory! Exiting...\n", exe_name, process_type, shm_name);
		}
	}
}





/* Function    :  cleanUp()
* Definition  :  Release all shared memory and delete all message queue, shared memory, and semaphore.*/
void cleanUp()
{
	//Delete [queue] shared memory
	if(g_mqueueid > 0)
	{
		msgctl(g_mqueueid, IPC_RMID, NULL);
	}

	//Release and delete [shmclock] shared memory
	discardShm(g_shmclock_shmid, g_shmclock_shmptr, "shmclock", g_exe_name, "Master");

	//Delete semaphore
	if(g_semid > 0)
	{
		semctl(g_semid, 0, IPC_RMID);
	}

	//Release and delete [pcbt] shared memory
	discardShm(g_pcbt_shmid, g_pcbt_shmptr, "pcbt", g_exe_name, "Master");
}




/* Function    :  semaLock()
* Definition  :  Invoke semaphore lock of the given semaphore and index.*/
void semaLock(int sem_index)
{
	g_sema_operation.sem_num = sem_index;
	g_sema_operation.sem_op = -1;
	g_sema_operation.sem_flg = 0;
	semop(g_semid, &g_sema_operation, 1);
}




/* Function    :  semaRelease()
* Definition  :  Release semaphore lock of the given semaphore and index.*/
void semaRelease(int sem_index)
{	
	g_sema_operation.sem_num = sem_index;
	g_sema_operation.sem_op = 1;
	g_sema_operation.sem_flg = 0;
	semop(g_semid, &g_sema_operation, 1);
}






/* Function    :  incShmclock()
* Definition  :  Increment the logical clock (simulation time).*/
void incShmclock()
{
	semaLock(0);

	//g_shmclock_shmptr->second++;
	g_shmclock_shmptr->nanosecond += rand() % 1000 + 1;
	if(g_shmclock_shmptr->nanosecond >= 1000000000)
	{
		g_shmclock_shmptr->second++;
		g_shmclock_shmptr->nanosecond = 1000000000 - g_shmclock_shmptr->nanosecond;
	}

	semaRelease(0);
}




/* Function    :  createQueue()
* Definition  :  A utility function to create an empty queue.*/
struct Queue *createQueue()
{
	struct Queue *q = (struct Queue *)malloc(sizeof(struct Queue));
	q->front = NULL;
	q->rear = NULL;
	return q;
}




/* Function    :  newNode()
* Definition  :  A utility function to create a new linked list node.*/ 
struct QNode *newNode(int index)
{ 
    struct QNode *temp = (struct QNode *)malloc(sizeof(struct QNode));
    temp->index = index;
    temp->next = NULL;
    return temp;
} 





/* Function    :  enQueue()
* Definition  :  The function to add a struct ProcessControlBlock to given queue.*/
void enQueue(struct Queue *q, int index) 
{ 
	//Create a new LL node
	struct QNode *temp = newNode(index);

	//If queue is empty, then new node is front and rear both
	if(q->rear == NULL)
	{
		q->front = q->rear = temp;
		return;
	}

	//Add the new node at the end of queue and change rear 
	q->rear->next = temp;
	q->rear = temp;
}





/* Function    :  deQueue()
* Definition  :  Function to remove a struct ProcessControlBlock from given queue.*/
struct QNode *deQueue(struct Queue *q) 
{
	//If queue is empty, return NULL
	if(q->front == NULL) 
	{
		return NULL;
	}

	//Store previous front and move front one node ahead
	struct QNode *temp = q->front;
	free(temp);
	q->front = q->front->next;

	//If front becomes NULL, then change rear also as NULL
	if (q->front == NULL)
	{
		q->rear = NULL;
	}

	return temp;
} 




/* Function    :  isQueueEmpty()
* Definition  :  Check if given queue is empty or not.*/
bool isQueueEmpty(struct Queue *q)
{
	if(q->rear == NULL)
	{
		return true;
	}
	else
	{
		return false;
	}
}




/* Function    :  initUserProcess()
* Definition  :  Sets up the user processes.*/
struct UserProcess *initUserProcess(int index, pid_t pid)
{
	struct UserProcess *user = (struct UserProcess *)malloc(sizeof(struct UserProcess));
	user->index = index;
	user->actualPid = pid;
	user->priority = 0;
	return user;
}




/* Function    :  userToPCB()
* Definition  :  Transfer the user process information to the process control block.*/
void userToPCB(struct ProcessControlBlock *pcb, struct UserProcess *user)
{
	pcb->pidIndex = user->index;
	pcb->actualPid = user->actualPid;
	pcb->priority = user->priority;
	pcb->lastBurst = 0;
	pcb->totalBurst = 0;
	pcb->totalSystemSecond = 0;
	pcb->totalSystemNanosecond = 0;
	pcb->totalWaitSecond = 0;
	pcb->totalWaitNanosecond = 0;
}
