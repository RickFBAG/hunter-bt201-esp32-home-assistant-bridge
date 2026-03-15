I have a Hunter BT201 watering system that only works over BLE. I want to build a local BLE-to-WiFi bridge with an ESP32 so I can control it from Home Assistant.

Context:
- The irrigation device is a Hunter BT201 / Hunter BTT.
- It currently only works when I am physically close to it over BLE.
- I want to use an ESP32 as the BLE client that talks to the Hunter device.
- I want Home Assistant to control the ESP32 over my local network.
- Home Assistant runs on a separate small NUC using HAOS (Home Assistant OS).
- The ESP32 hardware is a Waveshare ESP32-C6 1.47 inch Touch Display Development Board.
- Ignore the touchscreen for now. Do not build a screen UI. Focus on reliability and headless operation.
- Everything should work fully local. No cloud dependency.

Important:
I have already reverse engineered the BLE protocol and attached files with findings and test scripts. Use those attached files as the source of truth. Do not invent packets, characteristics, or behavior that is not grounded in the provided files. If something is still uncertain, clearly mark it as an assumption and isolate it behind a feature flag, TODO, or clearly documented limitation.

What I want you to build:
I want a production-minded first version of the full solution:
1. ESP32 firmware
2. Home Assistant integration
3. Setup instructions
4. Clear architecture explanation
5. Test plan
6. Mapping from Home Assistant entities/services to BLE operations

Architecture requirements:
- Choose the simplest and most reliable architecture.
- Prefer a fully local setup.
- Prefer maintainability over cleverness.
- Explicitly explain which architecture you chose and why.
- If there are multiple valid options, choose one and explain why you rejected the others.
- Reliability and safety matter more than elegance.

Functional requirements:
The system should expose the same practical controls I use in the Hunter app, at least for these features:
- Zone 1 on/off
- Zone 1 manual duration
- Zone 1 timer schedule
- Zone 1 cycling schedule
- Zone 2 on/off
- Zone 2 manual duration
- Zone 2 timer schedule
- Zone 2 cycling schedule
- Battery percentage

Home Assistant requirements:
Expose these in Home Assistant in a clean and usable way:
- Zone 1 start/stop
- Zone 1 duration selector, but hard-limited to max 1 hour
- Zone 1 timer configuration
- Zone 1 cycling configuration
- Zone 2 start/stop
- Zone 2 duration selector, but hard-limited to max 1 hour
- Zone 2 timer configuration
- Zone 2 cycling configuration
- Battery percentage sensor
- Optional low battery alert is nice to have, but not required for the first stable version

Safety and reliability requirements:
This is the most important part.
BLE is unstable, so the system must be built defensively.

Hard safety rules:
- The system must never allow watering for longer than 1 hour.
- Firmware must reject any duration above 3600 seconds.
- Home Assistant UI must also prevent anything above 1 hour.
- If there is any uncertainty about device state, do not continue watering blindly.
- On reboot, WiFi reconnect, BLE reconnect, timeout, or unexpected error, fail safe.
- Fail safe means: do not send new watering commands automatically, do not resume an old command queue, and do not assume watering is active unless confirmed.
- Stop must always be treated as high priority.
- Stop must be safe to send multiple times.
- If a command is not confirmed properly, retry safely.
- Do not create a situation where the valve could stay open because of bad retry logic.

Protocol handling requirements:
Use the reverse engineered protocol from the attached files.

Known protocol details that should be used exactly as provided:
- Custom service: FF80 with characteristics FF81 through FF8F
- Battery service: 180F with battery level 2A19
- On connect, notifications should be enabled for FF82, FF8A, and FF8F
- FF83 is the manual command channel
- FF84 appears to be time sync
- FF86 is used for Zone 1 related config / duration / schedule-day settings
- FF87 is used for a Zone 1 schedule block
- FF8B is used for Zone 2 related config / duration / schedule-day settings
- FF8D is used for a Zone 2 schedule block
- FF81 is used for passcode related data

Manual watering behavior:
Use the known working pattern from the attached files:
- prepare command
- write duration
- short delay
- arm/start command
- when stopping, use the known stable stop command
- stopping may require sending stop twice, because that appears in the captures and tests

Validation logic:
Do not just “fire and forget” BLE writes.
Use confirmation logic based on the observed notifications and reads.

At minimum:
- After start, verify expected state progression using notifications, especially FF8A countdown behavior if available
- After stop, verify stop confirmation from notifications, especially FF82 behavior if available
- If validation fails, retry safely
- Retries must be bounded and logged
- If bounded retries fail, surface an error state to Home Assistant instead of guessing success

Design requirements:
I want this built as a proper small system, not as a hacky demo script.

Please structure the solution with:
- a BLE transport layer
- a protocol layer
- a state machine / command coordinator
- a Home Assistant facing API or bridge layer
- explicit logging
- clear error handling
- clear separation between “known confirmed state” and “desired state”

State model:
Please define a simple but robust state model, for example:
- disconnected
- idle
- starting
- running
- stopping
- error
- unknown

And explain:
- how transitions happen
- how timeouts are handled
- how retries are handled
- what Home Assistant sees when state is uncertain

Battery requirements:
The Hunter device runs on 2 AA batteries.
That means:
- keep BLE connections as short and infrequent as practical
- do not poll aggressively
- only connect when needed
- if background reads are needed, keep them minimal
- explain your strategy to balance reliability and battery usage

Scheduling requirements:
Implement support for both of the following schedule types on both zones:
- Zone 1 timer schedule
- Zone 1 cycling schedule
- Zone 2 timer schedule
- Zone 2 cycling schedule

Important:
Some schedule parts may be more certain than others in the attached reverse engineering.
Use the attached files carefully.
If any schedule format is still partly uncertain:
- implement only the parts that are well supported by the evidence
- document the uncertain parts clearly
- do not fake completeness
- but still aim to support both timer and cycling for both zones if the reverse engineered protocol evidence is sufficient

For timer/cycling UI:
- expose only the fields that can be backed by the protocol evidence
- if needed, leave more advanced editing as a clearly documented follow-up

Touchscreen:
Ignore it completely for now.
Do not spend time on any display code, LVGL, touch interaction, or local UI.

What I want from you in the output:
Please do all of the following:
1. Propose the final architecture and explain why
2. Explain the data flow from Home Assistant to ESP32 to Hunter BT201
3. Explain how command confirmation and retries work
4. Explain how fail-safe behavior works
5. Provide the complete code
6. Provide setup instructions for the ESP32
7. Provide setup instructions for Home Assistant
8. Provide a protocol mapping table between Home Assistant actions and BLE operations
9. Clearly list assumptions, uncertainties, and follow-up items
10. Provide a realistic test plan with success/failure cases

Output style:
- Be practical
- Be explicit
- Do not hand-wave uncertain protocol parts
- Do not say something “should work” without explaining why
- If you make an assumption, say so clearly
- Use the attached files as evidence
- Prioritize reliability and safety over feature completeness

Acceptance criteria for the first usable version:
- I can start and stop Zone 1 from Home Assistant
- I can start and stop Zone 2 from Home Assistant
- I can set a manual runtime for both zones, but never above 1 hour
- Battery percentage is visible in Home Assistant
- Timer scheduling is implemented for both Zone 1 and Zone 2 if supported well enough by the reverse engineered data
- Cycling scheduling is implemented for both Zone 1 and Zone 2 if supported well enough by the reverse engineered data
- Start and stop are validated, not blind
- Stop is safe and repeatable
- The solution fails safe on disconnects, timeouts, and unexpected errors
- The solution is documented well enough that I can maintain it later

