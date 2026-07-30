# EEE4775 - Final Capstone Project - Theme Park Ride Tracker
## Overview
My final capstone project is a safety critical system that simulates a ride's progression and emergency stops, built to demonstrate ride controls for a theme park engineering role. It is based on App 5, which has a producer, consumer, coordinator, and responder tasks. The producer generates the current state, which is passed to the consumer and to the coordinator. The coordinator will trigger the responder, which provides the ride's status (active or halted). The responder can also be triggered by pressing the blue button, as in App 5. There is an additional red button to provide emergency stop functionality, and this will either halt or re-activate the ride. Finally, there is a web dashboard displaying the ride status, number of rides completed, and the percentage completion of the current ride. 
## Video Demo
<iframe width="640" height="360" src="https://www.youtube.com/embed/LNYvpyLEDYs" title="YouTube video player" frameborder="0" allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture" allowfullscreen></iframe>  

## Website Structure
The structure of the website is listed below. 
```
├── docs/                    # Root of Pages site
│   ├── index.html           # Overview & Demo Video
│   ├── architecture         # System diagram & explanation
│   ├── task_table           # Task table + WCET evidence
│   ├── hazard-analysis      # Hazard analysis
│   └── assets/              # Images / screenshots
│   └── reflection/          # Course reflection
│   └── about/               # Same as README, but rendered
├── firmware/                # Folder for Wokwi app
│   ├── src/
│   └── diagram.json         # Wokwi component diagram
└── README.md                # Full README (rendered on the site too)
```