#!/bin/bash

# Function to recursively call the script
function nested_call() {
    local level=$1
    local max_level=10

    # Check if the current nesting level is less than the maximum
    if [ "$level" -lt "$max_level" ]; then
        echo "Level $level"
        # Call the script again with an incremented nesting level
        "$0" $(($level + 1))
    else
        # Execute the desired command at the final nesting level
        echo "Reached maximum nesting level: $level"
        echo "Running sleep"
        sleep 3000
    fi
}

# Start the nesting with level 1
nested_call ${1:-1}
