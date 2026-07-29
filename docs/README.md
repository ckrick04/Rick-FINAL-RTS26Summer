# Theme Park Ride Monitor — Real-Time Systems Final Capstone

## One sentence description
A safety critical system that simulates a ride's progression and emergency stops, built to demonstrate ride controls for a theme park engineering role.

## Demo
- Video: <YouTube>
- Wokwi URL: https://wokwi.com/projects/470898166033667073
- Live Wokwi: Rick-FINAL-RTS26Summer

## Architecture
<diagram + 2–3 sentences on the data/control flow>

## Tasks & timing (WCET evidence)
| Task | Period T | WCET C | U=C/T | Priority | Deadline |
|------|---------:|-------:|------:|---------:|---------:|
<rows from the calculator>
Total utilization U = <value>  (RM bound / EDF feasible: <note>)

## Hazard analysis & standard mapping
<hazard, effect, mitigation; mapped to the standard clause>

## Graceful degradation
<what fails, how it is detected, what the system does instead>

## Build & Run
The program was created entirely through Wokwi. To run on Wokwi, the program ZIP file can be uploaded as a new Wokwi project. This includes both the code and the board with wired components. Since the program generates a WiFi dashboard, WokwiGW must be downloaded from the Wokwi website for the user to run it. Before running the program, make sure WokwiGW is opened and leave it in the background. With the setup complete, run the program within Wokwi, and open localhost:9080 in the browser to view the web dashboard.

## Tailored for
<target role> — <why these choices fit that role>