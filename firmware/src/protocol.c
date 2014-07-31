/*
  protocol.h - Lasersaur protocol parser.
  Part of LasaurApp

  Copyright (c) 2014 Stefan Hechenberger

  LasaurApp is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version. <http://www.gnu.org/licenses/>

  LasaurApp is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
*/


/* 

The Lasersaur Protocol
----------------------

The protocol is a ascii/binary hybrid. Markers are printable
ascii values while binary data is transmitted in the extended 
ascii range [128,255].

A transmitted byte can either be a command, a parameter, or a
partial binary number (data). Four bytes encode a number.
Parameters need to be set before sending the command that uses them.
Similarly the number needs to be set before sending the parameter
marker. This inverse transmission makes the parser super simple.

For example to send a line command:
<number>x<number>yB

Numbers are four bytes with values in the extended ascii range [128,255].
They are fixed-point floats with 3 decimals in the range of
[-134217.728, 134217.727]. For details how they are encoded see:
get_curent_value()


*/


#include <string.h>
#include <math.h>
#include "errno.h"
#include <stdint.h>
#include <stdlib.h>
#include <avr/pgmspace.h>
#include "protocol.h"
#include "config.h"
#include "serial.h"
#include "planner.h"
#include "stepper.h"
#include "sense_control.h"



#define REF_RELATIVE 0
#define REF_ABSOLUTE 1

#define OFFSET_TABLE 0
#define OFFSET_CUSTOM 1

#define PARAM_MAX_DATA_LENGTH 4


typedef struct {
  uint8_t ref_mode;                // {REF_RELATIVE, REF_ABSOLUTE}
  double feedrate;                 // mm/min {F}
  uint8_t intensity;               // 0-255 percentage
  double position[3];              // projected position once all scheduled motions will have been executed
  double duration;                 // pierce duration
  double pixel_width;              // raster pixel width in mm
  uint8_t offselect;               // {OFFSET_TABLE, OFFSET_CUSTOM}
  double target[3];                // X,Y,Z params accumulated
  double offsets[6];               // coord system offsets [table_X,table_Y,table_Z, custom_X,custom_Y,custom_Z]
} state_t;
static state_t st;

typedef struct {
  uint8_t chars[PARAM_MAX_DATA_LENGTH];
  uint8_t count;
} data_t;
static data_t pdata;

// uint8_t line_checksum_ok_already;
static volatile bool position_update_requested;  // make sure to update to stepper position on next occasion
static volatile bool status_requested;           // when set protocol_idle will write status to serial
static bool machine_idle;                        // when no serial data and no blockes processing

static void on_cmd(uint8_t command);
static void on_param(uint8_t parameter);
static double get_curent_value();


void protocol_init() {
  st.ref_mode = REF_ABSOLUTE;
  st.feedrate = CONFIG_FEEDRATE;
  st.intensity = 0;
  clear_vector(st.position);
  clear_vector(st.target);
  st.duration = 0.0;
  st.pixel_width = 0.0;
  st.offselect = OFFSET_TABLE;
  // table offset
  st.offsets[X_AXIS] = CONFIG_X_ORIGIN_OFFSET;
  st.offsets[Y_AXIS] = CONFIG_Y_ORIGIN_OFFSET;
  st.offsets[Z_AXIS] = CONFIG_Z_ORIGIN_OFFSET;
  // custom offset
  st.offsets[3+X_AXIS] = CONFIG_X_ORIGIN_OFFSET;
  st.offsets[3+Y_AXIS] = CONFIG_Y_ORIGIN_OFFSET;
  st.offsets[3+Z_AXIS] = CONFIG_Z_ORIGIN_OFFSET;
  position_update_requested = false;
  // line_checksum_ok_already = false;
  status_requested = true;
  machine_idle = true;
}


void protocol_loop() {
  uint8_t chr;
  while(true) {
    chr = serial_protocol_read();  // blocks until there is data

    // handle position update after a stop
    if (position_update_requested) {
      st.position[X_AXIS] = stepper_get_position_x();
      st.position[Y_AXIS] = stepper_get_position_y();
      st.position[Z_AXIS] = stepper_get_position_z();
      position_update_requested = false;
    }

    if (stepper_stop_requested()) {
      protocol_request_status();
    } else {
      if(chr < 128) {  /////////////////////////////// marker
        if(chr > 64 && chr < 91) {  ///////// command
          // chr is in [A-Z]
          on_cmd(chr);
        } else if(chr > 96 && chr < 123) {  //parameter
          // chr is in [a-z]
          on_param(chr);
        } else {
          stepper_request_stop(STOPERROR_INVALID_MARKER);
        }
        pdata.count = 0;
      } else {  //////////////////////////////////////// data
        // chr is in [128,255]
        if(pdata.count < PARAM_MAX_DATA_LENGTH) {
          pdata.chars[pdata.count++] = chr;
        } else {
          stepper_request_stop(STOPERROR_INVALID_DATA);
        }
      }
    }

    protocol_idle();
    serial_write('\n');
  }
}



inline void on_cmd(uint8_t command) {
  uint8_t cs;
  switch(command) {
    case CMD_NONE:
      break;
    case CMD_LINE: case CMD_RASTER:
        if(command == CMD_RASTER) {
          planner_line( st.target[X_AXIS] + st.offsets[3*st.offselect+X_AXIS], 
                        st.target[Y_AXIS] + st.offsets[3*st.offselect+Y_AXIS], 
                        st.target[Z_AXIS] + st.offsets[3*st.offselect+Z_AXIS], 
                        st.feedrate, st.intensity, st.pixel_width );
        } else {
          planner_line( st.target[X_AXIS] + st.offsets[3*st.offselect+X_AXIS], 
                        st.target[Y_AXIS] + st.offsets[3*st.offselect+Y_AXIS], 
                        st.target[Z_AXIS] + st.offsets[3*st.offselect+Z_AXIS], 
                        st.feedrate, st.intensity, 0 );
        }
      break;
    case CMD_DWELL:
      planner_dwell(st.duration, st.intensity);
      break;
    case CMD_SET_FEEDRATE:
      st.feedrate = get_curent_value();
      break;
    case CMD_SET_INTENSITY:
      st.intensity = get_curent_value();
      break;
    case CMD_REF_RELATIVE:
      st.ref_mode = REF_RELATIVE;
      break;
    case CMD_REF_ABSOLUTE:
      st.ref_mode = REF_ABSOLUTE;
      break;
    case CMD_HOMING:
      stepper_homing_cycle();
      // now that we are at the physical home
      // zero all the position vectors
      clear_vector(st.position);
      clear_vector(st.target);
      planner_set_position(0.0, 0.0, 0.0);
      // move head to table offset
      st.offselect = OFFSET_TABLE;
      st.target[X_AXIS] = 0.0;
      st.target[Y_AXIS] = 0.0;
      st.target[Z_AXIS] = 0.0;         
      planner_line( st.target[X_AXIS] + st.offsets[3*st.offselect+X_AXIS], 
                    st.target[Y_AXIS] + st.offsets[3*st.offselect+Y_AXIS], 
                    st.target[Z_AXIS] + st.offsets[3*st.offselect+Z_AXIS], 
                    st.feedrate, 0, 0 );
      break;
    case CMD_SET_OFFSET_TABLE: case CMD_SET_OFFSET_CUSTOM:
      // set offset to current position
      cs = OFFSET_TABLE;
      if(command == CMD_SET_OFFSET_CUSTOM) {
        cs = OFFSET_CUSTOM;
      }
      st.offsets[3*cs+X_AXIS] = st.position[X_AXIS] + st.offsets[3*st.offselect+X_AXIS];
      st.offsets[3*cs+Y_AXIS] = st.position[Y_AXIS] + st.offsets[3*st.offselect+Y_AXIS];
      st.offsets[3*cs+Z_AXIS] = st.position[Z_AXIS] + st.offsets[3*st.offselect+Z_AXIS];
      st.target[X_AXIS] = 0.0;
      st.target[Y_AXIS] = 0.0;
      st.target[Z_AXIS] = 0.0;   
      break;
    case CMD_DEF_OFFSET_TABLE: case CMD_DEF_OFFSET_CUSTOM:
      // set offset to target
      cs = OFFSET_TABLE;
      if(CMD_SET_OFFSET_CUSTOM) {
        cs = OFFSET_CUSTOM;
      }
      st.offsets[3*cs+X_AXIS] = st.target[X_AXIS];
      st.offsets[3*cs+Y_AXIS] = st.target[Y_AXIS];
      st.offsets[3*cs+Z_AXIS] = st.target[Z_AXIS];
      // Set target in ref to new coord system so subsequent moves are calculated correctly.
      st.target[X_AXIS] = (st.position[X_AXIS] + st.offsets[3*st.offselect+X_AXIS]) - st.offsets[3*cs+X_AXIS];
      st.target[Y_AXIS] = (st.position[Y_AXIS] + st.offsets[3*st.offselect+Y_AXIS]) - st.offsets[3*cs+Y_AXIS];
      st.target[Z_AXIS] = (st.position[Z_AXIS] + st.offsets[3*st.offselect+Z_AXIS]) - st.offsets[3*cs+Z_AXIS];
      break;
    case CMD_SEL_OFFSET_TABLE: 
      st.offselect = OFFSET_TABLE;
      break;
    case CMD_SEL_OFFSET_CUSTOM:
      st.offselect = OFFSET_CUSTOM;
      break;
    case CMD_AIR_ENABLE:
      planner_control_air_assist_enable();
      break;
    case CMD_AIR_DISABLE:
      planner_control_air_assist_disable();
      break;
    case CMD_AUX1_ENABLE:
      planner_control_aux1_assist_enable();
      break;
    case CMD_AUX1_DISABLE:
      planner_control_aux1_assist_disable();
      break;
    case CMD_AUX2_ENABLE:
      planner_control_aux2_assist_enable();
      break;
    case CMD_AUX2_DISABLE:
      planner_control_aux2_assist_disable();
      break;   
    default:
      stepper_request_stop(STOPERROR_INVALID_COMMAND);
  }

  memcpy(st.position, st.target, sizeof(st.target));  //position = target
}



inline void on_param(uint8_t parameter) {
  if(pdata.count == 4) {
    switch(parameter) {
      case PARAM_TARGET_X:
        st.target[X_AXIS] = get_curent_value();
        break;
      case PARAM_TARGET_Y:
        st.target[Y_AXIS] = get_curent_value();
        break;
      case PARAM_TARGET_Z:
        st.target[Z_AXIS] = get_curent_value();
        break;
      case PARAM_FEEDRATE:
        st.feedrate = get_curent_value();
        break;
      case PARAM_INTENSITY:
        st.intensity = get_curent_value();
        break;
      case PARAM_DURATION:
        st.duration = get_curent_value();
        break;
      case PARAM_PIXEL_WIDTH:
        st.pixel_width = get_curent_value();
        break;
      default:
        stepper_request_stop(STOPERROR_INVALID_PARAMETER);
    }
  } else {
    stepper_request_stop(STOPERROR_INVALID_DATA);
  }
}


void protocol_request_status() {
  status_requested = true;
}


void protocol_end_of_job_check() {
  if(!planner_blocks_available()) {
    // probably end of job
    // (can also be the serial is super slow)
    machine_idle = true;
  }
}


void protocol_idle() {
  // Continuously called in protocol_loop
  // Also called when the protocol loop is blocked by
  // one of the following conditions:
  // - serial reading
  //   - in raster mode
  //   - serial buffer empty
  // - planing actions (line, command)
  //   - block buffer full

  if (planner_blocks_available()) {
    machine_idle = false;
  }

  if (status_requested) {
    // idle flag
    if (machine_idle) {
      serial_write(INFO_IDLE_YES);
    } else {
      serial_write(INFO_IDLE_NO);
    }

    // Handle STOPERROR conditions
    uint8_t stop_code = stepper_stop_status();
    if (stop_code != STOPERROR_OK) {
      // report a stop error
      serial_write(stop_code);
      // always report limits
      if (SENSE_X1_LIMIT && stop_code != STOPERROR_LIMIT_HIT_X1) {
        serial_write(STOPERROR_LIMIT_HIT_X1);
      }
      if (SENSE_X2_LIMIT && stop_code != STOPERROR_LIMIT_HIT_X2) {
        serial_write(STOPERROR_LIMIT_HIT_X2);
      }
      if (SENSE_Y1_LIMIT && stop_code != STOPERROR_LIMIT_HIT_Y1) {
        serial_write(STOPERROR_LIMIT_HIT_Y1);
      }
      if (SENSE_Y2_LIMIT && stop_code != STOPERROR_LIMIT_HIT_Y2) {
        serial_write(STOPERROR_LIMIT_HIT_Y2);
      }
      if (SENSE_Z1_LIMIT && stop_code != STOPERROR_LIMIT_HIT_Z1) {
        serial_write(STOPERROR_LIMIT_HIT_Z1);
      }
      if (SENSE_Z2_LIMIT && stop_code != STOPERROR_LIMIT_HIT_Z2) {
        serial_write(STOPERROR_LIMIT_HIT_Z2);
      }
    }

    // position
    serial_write_number(stepper_get_position_x());
    serial_write(STATUS_POS_X);
    serial_write_number(stepper_get_position_y());
    serial_write(STATUS_POS_Y);
    serial_write_number(stepper_get_position_z());       
    serial_write(STATUS_POS_Z);
    // version
    serial_write_number(LASAURGRBL_VERSION);       
    serial_write(STATUS_VERSION);

    status_requested = false;
  }
}


void protocol_request_position_update() {
  position_update_requested = true;
}


inline double get_curent_value() {
  // returns a number based on the current data chars
  // chars expected to be extended ascii [128,255]
  // 28bit total, three decimals are restored
  // number is in [-134217.728, 134217.727] 
  //
  // The encoding in Python works like this:
  //// num = int(round( (num*1000) + (2**27)))
  //// char0 = (num&127)+128
  //// char1 = ((num&(127<<7))>>7)+128
  //// char2 = ((num&(127<<14))>>14)+128
  //// char3 = ((num&(127<<21))>>21)+128
  return (((( pdata.chars[3]-128)*2097152 +  // 2097152 = 128*128*128
              (pdata.chars[2]-128)*16384 +   //   16384 = 128*128
              (pdata.chars[1]-128)*128 + 
              (pdata.chars[0]-128))-134217728 ) / 1000.0);  // 134217728 = 2**27
}





// inline double num_from_chars(uint8_t char0, uint8_t char1, uint8_t char2, uint8_t char3) {
//   // chars expected to be extended ascii [128,255]
//   // 28bit total, three decimals are restored
//   // number is in [-134217.728, 134217.727] 
//   return ((((char3-128)*2097152+(char2-128)*16384+(char1-128)*128+(char0-128))-134217728)/1000.0);
// }

// inline void chars_from_num(num, uint8_t* char0, uint8_t* char1, uint8_t* char2, uint8_t* char3) {
//   // num to be [-134217.728, 134217.727]
//   // three decimals are retained
//   uint32_t num = lround(num*1000 + 134217728);
//   char0 = (num&127)+128
//   char1 = ((num&(127<<7))>>7)+128
//   char2 = ((num&(127<<14))>>14)+128
//   char3 = ((num&(127<<21))>>21)+128
//   return char3, char2, char1, char0
// }

// IN PYTHON
// def double_from_chars_4(char3, char2, char1, char0):
//     # chars expected to be extended ascii [128,255]
//     return ((((char3-128)*128*128*128 + (char2-128)*128*128 + (char1-128)*128 + (char0-128) )- 2**27)/1000.0)
//
// def chars4_from_double(num):
//     # num to be [-134217.728, 134217.727]
//     # three decimals are retained
//     num = int(round( (num*1000) + (2**27)))
//     char0 = (num&127)+128
//     char1 = ((num&(127<<7))>>7)+128
//     char2 = ((num&(127<<14))>>14)+128
//     char3 = ((num&(127<<21))>>21)+128
//     return char3, char2, char1, char0
//
// def check(val):
//     char3, char2, char1, char0 = chars4_from_double(val)
//     val2 = double_from_chars_4(char3, char2, char1, char0)
//     print "assert %s == %s" % (val, val2)
//     # assert val == val2
//
// check(13925.2443)

