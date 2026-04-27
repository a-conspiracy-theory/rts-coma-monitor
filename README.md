Company Synopsys
    	I chose to implement a healthcare product. This company specializes in unresponsive patient care, as many elderly patients in Florida ultimately fall in to some sort of unresponsive state before their final passing. This project aims to monitor patients while they are in this state, and if their vital signs exceed threshold values, call an emergency code to their room. We aim to assist nurses in their care of unresponsive patients.

AI USE:
    Debugging - determined my logical vs bitwise OR in line 603 was causing a latching effect with temp and oxygen sensor tasks.
    Code - initial help with writing the webpage, It taught me how to use HTML and how to use inserted characters

DISUSE OF WOKWI:
    When trying to set up the wokwi web server in APP#5, it said i had to pay to use it. Rather than do this i used my own physical esp32 (laying around) to create a true real-time system with an actual web interface. Because of this, I am unable to use the "Wokwi Logic Analyze". I hope that you will be kind in grading and not give partial credit for something that i cannot utilize. 



Scheduler Fit: How do your task priorities / RTOS settings guarantee every H task’s deadline in Wokwi? Cite one timestamp pair that proves it.

    In this project, there is only one hard-deadline task: the event handler. the other three tasks are all sensors, and while it is important that they execute on-time, it is not as crucial as the event handler. As such, It has the highest priority of the non-wifi tasks. If something happens, it has authority to block all other standard tasks. This is proven by my sensor spam test. Even when the system is so overloaded that the heartbeat LED cannot function and other level-2 tasks are getting starved for system resources, the event handler run time never crosses 1 tick of run time. 
    AMMENDMENT: event-handler is now priority 5, the maximum of the whole system.


Race‑Proofing: Where could a race occur? Show the exact line(s) you protected and which primitive solved it.

    There are many locations where a race could occur, by far the most obvious one being the led_state variable. This variable is modified and read by both high- and low-priority tasks, and as such race conditions are a genuine threat. In line 259, if the wifi task preempts the event handler while it is modifying the LED state, it could read corrupted data and cause the webpage to fail. I wrapped both the write and read in a mutex semaphore to prevent this from occuring.


Worst‑Case Spike: Describe the heaviest load you threw at the prototype (e.g., sensor spam, comm burst). What margin (of time) remained before an H deadline would slip?

    With the help of Google Gemini, I wrote a "comms hyperspam" script that violated my esp32 with 20 separate repeat ping commands of the "A" character (to simulate screaming because i think it is funny). This unfortunately used up to 90% of my desktop CPU, and deadlines still were not breaking. i moved to my linux laptop with the [sudo hping3 --flood --icmp -d 1450 ESP32_IP] command. almost immediately deadlines were broken. At a tick rate of 1000hz, even my priority 3 eventhandler task was taking over 2000ms in certain cases. I increased the eventhandler task to priority 5 and wifi to 3,4. Even doing this did not fix the issue. I believe the wifi interrupts, due to the separate space of ISRs, are overriding the handler task even with the highest priority. the watchdog timer was eventually triggered. with my shiny new 1khz tick time, I tried a 2ms sensor task period. I was able to see the heartrate task ever so slighly fall behind, by a few ms per beat.


Design Trade‑off: Name one feature you didn’t add (or simplified) to keep timing predictable. Why was that the right call for your chosen company?

    one feature that was greatly simplified was the webpage interface. For this task, a "pretty" webpage is not required. Additionally, ALL emergency alterts come through the LED immediately, assuming no wifi spam, and can be addressed by the push button at ISR speed. While a one second delay could be an issue if there was no other signaling/confirmation device, both exist in this case. Functional software is generally not known for being eye candy.


PROTOTYPE REQUIREMENTS
        Hardware
D            System Heartbeat - LED
D            "Critical Condition" - LED

D            Patient heartbeat monitor - Varistor sensor

D            Patient emergency button - momentary input - toggle critical state
        4 tasks
D            Patient Heartbeat Monitor (implemented with a varistor)
D            Patient Temperature Sensor (implemented with a modified random walker)
D            Dissolved Oxygen Sensor (implemented with a modified random walker)
D            Event Handler Task (this task is variable)

        1 ISR
D            Patient Emergency Button

        Timing
            event handler : hard : 50ms
            patient temperature sensor : soft : 200ms
            patient heartbeat monitor : soft : 100ms
            dissolved oxygen sensor : soft : 100ms
        
        Sync
D            Mutex - LED state access - required because different tasks at different priorities read and write to this variable
D            Binary Sem - Button ISR semaphore - required because doing anything but calling a semaphore in an ISR is bad

        Inter-Task, external Comms
D            Wifi - patient monitoring portal, button for critical state toggle
D            "HR Sensor Data" Queue - hr sense task pushes data to a queue, event handler task transforms it into a data buffer for webpage.
D            "critical condition" event group. hr, tmp, oxy bits. if any are outside of normal ranges, respective task sets the bit.

        Determinism Proof
            "event handler" task duration readout
D            System Heartbeat LED with timestamp

        Company Context
            As described. Temperature, Heartbeat, and Oxygen sensors, patient stability portal, emergency call button, etc.
