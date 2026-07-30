## Task Table

Using the WCET measurement tool from previous apps, I found the WCET for the tasks in the system, which is shown below.  

![WCET Measurements](./assets/FINAL_WCET.png)  

| Task | Function | Period (ms) | WCET (ms) | Deadline (ms) | Priority | Core |
| ---- | -------- | ----------- | --------- | ------------- | -------- | ---- |
| Producer | Produce the current ride status | 100 | 0.033 | 100 | 8 | 1 |  
| Consumer | Consume and display ride status from queue | 100 | 1.682 | 100 | 8 | 1 |  
| Coordinator | Check that both producer and consumer completed | 100 | 4.134 | 100 | 9 | 1 |  
| Responder | Display if ride is active or has stopped | Sporadic | 4.062 | 20 | 12 | 1 |  
| Emergency Stop | Halt the ride by suspending the producer | Sporadic | 1.576 | 10 | 15 | 1 |  

Utilization: 0.033 / 100 + 1.682 / 100 + 4.134 / 100 + 4.062 / 20 + 1.576 / 10  
U = 0.42  
The program is feasible under RMS (bound = 0.743 for 5 tasks) and under EDF (under 1).