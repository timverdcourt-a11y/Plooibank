## Repository structure
 
```
Plooibank/
├── DisplayGUI_folder/        # Graphical interface shown on the display + main logic
├── DisplayInput_folder/      # Handling of operator input
└── MotorController_folder/   # Motor control for raising/lowering the stop
```
 
### DisplayGUI
Renders the on-screen interface: the current and target height of the stop, status information, and feedback to the operator.
This contains the main operating logic
 
### DisplayInput
Reads and processes input from the operator (entering a target height, starting/stopping a move) and passes it on to the rest of the system.
 
### MotorController
Drives the motor that adjusts the height of the stop. This module translates the requested height into actual motor movement and positioning.

## AI assistance 
Portions of the code in this repository were developed with the assistance of AI tools.
