When a sample is played, it is created from three channels:
* current (straight from adc)
* active (the loop that can be undone/redone)
* main (the main loop)

### state - INIT
* no playback is occuring

when the button is pressed, the state changes to `FIRST_RECORD`

### state - FIRST_RECORD
* the loop length is currently being determined
* recording is occuring, so PSRAM is written to
* active buffer is being used. main buffer is not.
* The first block should always be buffered, so that it can be played as soon as the player presses the button

when the button is held, the state changes to `INIT`
when the button is pressed again, the loop is completed and the state changes to `RECORD_OVER`
when the button is pressed twice, the loop is completed and the state changes to `PLAYBACK`

### state - PLAYBACK
* the loop is being played back
* recording is not occuring, so PSRAM is not written to

when the button is held, the state changes to `INIT`
when the button is pressed again, the state changes to `RECORD_OVER`

### state - RECORD_OVER
* the loop is being played back
* recording is occuring, so PSRAM is written to
* The active buffer is added into the main buffer, while the current recording is added
to the active buffer.

when the button is pressed again, the loop is completed and the state changes to `PLAYBACK`