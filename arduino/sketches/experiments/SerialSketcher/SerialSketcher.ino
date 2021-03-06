/**
 * Copyright (c) 2015 Andrew Rapp. All rights reserved.
 *
 * This file is part of arduino-sketcher
 *
 * arduino-sketcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * arduino-sketcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with arduino-sketcher.  If not, see <http://www.gnu.org/licenses/>.
 */
 
#include <Wire.h>

/* CONFIGURATION

// This may be useful for cases where you want to program an Arduino (optiboot) that doesn't have a usb port (Arduino Pro, breadboard arduino etc), 
// and you don't have a FTDI cable/board handy. In this situation, wire the Arduino (to be programmed) to a usb Arduino and send to program
// The programmer Arduino must be a Leonardo or other Arduino variant with at least 2 serial ports, as the Java "loader" needs the usb-serial
// port to send the program and the UART must be connected to the other Arduino. If you do not have a Leonardo it a FTDI could be used, but if you
// have that then you could just use that to program the Arduino!

// This sketch "uploads" a sketch to a Arduino (with Optiboot) via Serial @ 115.2K

// Programmer: Arduino Leonardo. Runs this Sketch
// Target: Arduino Diecimila (168) with Optiboot 5.0a. Could be any optiboot enabled Arduino

Wiring: Programmer->Target
// GND->GND
// 5V->5V
// TX->RX
// RX->TX
// Programmer (D8) -> Target (Reset)
// Programer USB -> Host
*/

// TODO write program to external eeprom
// TODO handle incomplete programming attempts
// send # of pages to expect in header

// See boards.txt (/Applications/Arduino.app//Contents/Resources/Java/hardware/arduino/boards.txt)
// for the avrdude configuration for the board/bootloader combo. This sketch only supports Optiboot targets

// only need 128=> + 4 bytes len/addr
#define BUFFER_SIZE 150
#define READ_BUFFER_SIZE 150

#define STK_OK              0x10
#define STK_FAILED          0x11  // Not used
#define STK_UNKNOWN         0x12  // Not used
#define STK_NODEVICE        0x13  // Not used
#define STK_INSYNC          0x14  // ' '
#define STK_NOSYNC          0x15  // Not used
#define ADC_CHANNEL_ERROR   0x16  // Not used
#define ADC_MEASURE_OK      0x17  // Not used
#define PWM_CHANNEL_ERROR   0x18  // Not used
#define PWM_ADJUST_OK       0x19  // Not used
#define CRC_EOP             0x20  // 'SPACE'
#define STK_GET_SYNC        0x30  // '0'
#define STK_GET_SIGN_ON     0x31  // '1'
#define STK_SET_PARAMETER   0x40  // '@'
#define STK_GET_PARAMETER   0x41  // 'A'
#define STK_SET_DEVICE      0x42  // 'B'
#define STK_SET_DEVICE_EXT  0x45  // 'E'
#define STK_ENTER_PROGMODE  0x50  // 'P'
#define STK_LEAVE_PROGMODE  0x51  // 'Q'
#define STK_CHIP_ERASE      0x52  // 'R'
#define STK_CHECK_AUTOINC   0x53  // 'S'
#define STK_LOAD_ADDRESS    0x55  // 'U'
#define STK_UNIVERSAL       0x56  // 'V'
#define STK_PROG_FLASH      0x60  // '`'
#define STK_PROG_DATA       0x61  // 'a'
#define STK_PROG_FUSE       0x62  // 'b'
#define STK_PROG_LOCK       0x63  // 'c'
#define STK_PROG_PAGE       0x64  // 'd'
#define STK_PROG_FUSE_EXT   0x65  // 'e'
#define STK_READ_FLASH      0x70  // 'p'
#define STK_READ_DATA       0x71  // 'q'
#define STK_READ_FUSE       0x72  // 'r'
#define STK_READ_LOCK       0x73  // 's'
#define STK_READ_PAGE       0x74  // 't'
#define STK_READ_SIGN       0x75  // 'u'
#define STK_READ_OSCCAL     0x76  // 'v'
#define STK_READ_FUSE_EXT   0x77  // 'w'
#define STK_READ_OSCCAL_EXT 0x78  // 'x'

uint8_t cmd_buffer[1];
uint8_t buffer[BUFFER_SIZE];
uint8_t read_buffer[READ_BUFFER_SIZE];

// disable or bootloader timesout due to delays between prog_page
#define VERBOSE false

// wiring:
const int ssTx = 4;
const int ssRx = 5;
const int resetPin = 8;

const int PROG_PAGE_RETRIES = 2;

// first byte in packet indicates if first or last
const int FIRST_PAGE = 0xd;
const int LAST_PAGE = 0xf;
const int PROG_PAGE = 0xa;

// max time between optiboot commands before we send a noop
const int MAX_OPTI_DELAY = 300;

uint8_t page_len = 0;
uint8_t pos = 0;

int count = 0;
bool prog_mode = false;
bool is_first_page = false;
bool is_last_page = false;
bool bounced = false;

long last_optiboot_cmd = 0;

//SoftwareSerial nss(ssTx, ssRx);

Stream* getProgrammerSerial() {
  return &Serial1;
}

Stream* getDebugSerial() {
//  return &nss;  
  return &Serial;
}

void clear_read() {
  int count = 0;
  while (getProgrammerSerial()->available() > 0) {
    getProgrammerSerial()->read();
    count++;
  }
  
  if (count > 0) {
    getDebugSerial()->print("Discarded "); getDebugSerial()->print(count, DEC); getDebugSerial()->println(" extra bytes");
  }
}

int read_response(uint8_t len, int timeout) {
  long start = millis();
  int pos = 0;
  
//  if (VERBOSE) {
    //getDebugSerial()->print("read_response() expecting reply len: "); getDebugSerial()->println(len, DEC);    
//  }

  while (millis() - start < timeout) {
    
//    getDebugSerial()->println("waiting for response");
          
    if (getProgrammerSerial()->available() > 0) {
      read_buffer[pos] = getProgrammerSerial()->read();
      
      // extra verbose
//      if (VERBOSE) {
//        getDebugSerial()->print("read_response()<-"); getDebugSerial()->println(read_buffer[pos], HEX);        
//      }

      pos++;
      
      if (pos == len) {
        // we've read expected len
        break;
      }
    }
  }
  
  // consume any extra
  clear_read();
  
  if (pos == len) {
    return pos;
  }
  
  getDebugSerial()->print("read_response() timeout! read "); getDebugSerial()->print(pos, DEC); getDebugSerial()->print(" bytes but expected "); getDebugSerial()->print(len, DEC); getDebugSerial()->println(" bytes");
  return -1;
}

void dump_buffer(uint8_t arr[], char context[], uint8_t offset, uint8_t len) {
  getDebugSerial()->print(context);
  // weird this crashes leonardo
  //getDebugSerial()->print("start at "); getDebugSerial()->print(offset, DEC); getDebugSerial()->print(" len "); getDebugSerial()->print(len, DEC);
  getDebugSerial()->print(": ");
  
  for (int i = offset; i < offset + len; i++) {
    getDebugSerial()->print(arr[i], HEX);
    
    if (i < (offset + len) -1) {
      getDebugSerial()->print(",");
    }
  }
  
  getDebugSerial()->println("");
  getDebugSerial()->flush();
}

void update_last_command() {
  last_optiboot_cmd = millis();  
}

// Send command and buffer and return length of reply
int send(uint8_t command, uint8_t arr[], uint8_t offset, uint8_t len, uint8_t response_length) {

    if (VERBOSE) {
      if (command == STK_GET_PARAMETER) {
        getDebugSerial()->print("send() STK_GET_PARAMETER: "); getDebugSerial()->println(command, HEX);  
      } else if (command == STK_ENTER_PROGMODE) {
        getDebugSerial()->print("send() STK_ENTER_PROGMODE: "); getDebugSerial()->println(command, HEX);          
      } else if (command == STK_LEAVE_PROGMODE) {
        getDebugSerial()->print("send() STK_LEAVE_PROGMODE: "); getDebugSerial()->println(command, HEX);  
      } else if (command == STK_LOAD_ADDRESS) {
        getDebugSerial()->print("send() STK_LOAD_ADDRESS: "); getDebugSerial()->println(command, HEX);  
      } else if (command == STK_PROG_PAGE) {
        getDebugSerial()->print("send() STK_PROG_PAGE: "); getDebugSerial()->println(command, HEX);  
      } else if (command == STK_READ_PAGE) {
        getDebugSerial()->print("send() STK_READ_PAGE: "); getDebugSerial()->println(command, HEX);  
      } else if (command == STK_READ_SIGN) {
        getDebugSerial()->print("send() STK_READ_SIGN: "); getDebugSerial()->println(command, HEX);  
      } else if (command == STK_GET_SYNC) {
        getDebugSerial()->print("send() STK_GET_SYNC: "); getDebugSerial()->println(command, HEX);  
      } else {
        getDebugSerial()->print("send() unexpected command: "); getDebugSerial()->println(command, HEX);          
      }
    }
    
    getProgrammerSerial()->write(command);

  if (arr != NULL && len > 0) {
    for (int i = offset; i < offset + len; i++) {
      getProgrammerSerial()->write(arr[i]);

//      if (VERBOSE) {
//        getDebugSerial()->print("send()->"); getDebugSerial()->println(arr[i], HEX);  
//      }      
    }
    
    if (VERBOSE) {
      dump_buffer(arr, "send()->", offset, len);
    }
  }
  
  getProgrammerSerial()->write(CRC_EOP);
  // make it synchronous
  getProgrammerSerial()->flush();
      
  // add 2 bytes since we always expect to get back STK_INSYNC + STK_OK
  int reply_len = read_response(response_length + 2, 5000);

  if (reply_len == -1) {
    return -1;
  }
  
  if (VERBOSE) {
    dump_buffer(read_buffer, "send_reply", 0, reply_len);    
  }

  if (reply_len < 2) {
    getDebugSerial()->println("Invalid response");
    return -1; 
  }

  if (read_buffer[0] != STK_INSYNC) {
    getDebugSerial()->print("Expected STK_INSYNC but was "); getDebugSerial()->println(read_buffer[0], HEX);
    return -1;
  }
  
  if (read_buffer[reply_len - 1] != STK_OK) {
    getDebugSerial()->print("Expected STK_OK but was "); getDebugSerial()->println(read_buffer[reply_len - 1], HEX);
    return -1;    
  }
  
  // rewrite buffer without the STK_INSYNC and STK_OK
  uint8_t data_reply = reply_len - 2;
  
  for (int i = 0; i < data_reply; i++) {
    read_buffer[i] = read_buffer[i+1];
  }
  
  // zero the ok
  read_buffer[reply_len - 1] = 0;
  
  // success update
  update_last_command();
  
  // return the data portion of the length
  return data_reply;
}

void bounce() {    
    // Bounce the reset pin
    getDebugSerial()->println("Bouncing the Arduino");
    // set reset pin low
    digitalWrite(resetPin, LOW);
    delay(200);
    digitalWrite(resetPin, HIGH);
    delay(300);
    bounced = true;
}

int initTarget() {
  clear_read();
    
    int data_len = 0;
    
    // Check everything we can check to ensure we are speaking to the correct boot loader
    cmd_buffer[0] = 0x81;
    data_len = send(STK_GET_PARAMETER, cmd_buffer, 0, 1, 1);
    
    if (data_len == -1) {
     return -1;   
    }
    
    getDebugSerial()->print("Firmware version is "); getDebugSerial()->println(read_buffer[0], HEX);
    
    cmd_buffer[0] = 0x82;
    data_len = send(STK_GET_PARAMETER, cmd_buffer, 0, 1, 1);

    if (data_len == -1) {
     return -1;   
    }

    getDebugSerial()->print("Minor is "); getDebugSerial()->println(read_buffer[0], HEX);    
    
    // this not a valid command. optiboot will send back 0x3 for anything it doesn't understand
    cmd_buffer[0] = 0x83;
    data_len = send(STK_GET_PARAMETER, cmd_buffer, 0, 1, 1);
    
    if (data_len == -1) {
      return -1;   
    } else if (read_buffer[0] != 0x3) {
      getDebugSerial()->print("Expected 0x3 but was "); getDebugSerial()->println(read_buffer[0]);    
      return -1;
    }

    data_len = send(STK_READ_SIGN, NULL, 0, 0, 3);
    
    if (data_len != 3) {      
      return -1;      
      // tomatoless expects a different signature than what I get from optiboot so this might not be effective verification
      // assert(signature.len() == 3 && signature[0] == 0x1E && signature[1] == 0x95 && signature[2] == 0x0F);
    } else if (read_buffer[0] != 0x1E && read_buffer[1] != 0x94 && read_buffer[2] != 0x6) {
      getDebugSerial()->println("Signature invalid");
      return -1;
    }
    
    // IGNORED BY OPTIBOOT
    // avrdude does a set device
    //avrdude: Send: B [42] . [86] . [00] . [00] . [01] . [01] . [01] . [01] . [03] . [ff] . [ff] . [ff] . [ff] . [00] . [80] . [02] . [00] . [00] . [00] @ [40] . [00]   [20]     
    // then set device ext
    //avrdude: Send: E [45] . [05] . [04] . [d7] . [c2] . [00]   [20]     
    return 0;
}

int send_page(uint8_t addr_offset, uint8_t data_len) {    
    // ctrl,len,addr high/low
    // address is byte index 2,3
    
    // FIXME retries don't work since we overwrite the address, oops
    // retry up to 2 times
    for (int z = 0; z < PROG_PAGE_RETRIES + 1; z++) {
      // [55] . [00] . [00] 
      if (send(STK_LOAD_ADDRESS, buffer, addr_offset, 2, 0) == -1) {
        getDebugSerial()->println("Load address failed");
        return -1;
      }
       
      // [64] . [00] . [80] F [46] . [0c] .
      
      // rewrite buffer to make things easier
      // data starts at addr_offset + 2
      // format of prog_page is 0x00, data_len, 0x46, [data]
      buffer[addr_offset - 1] = 0;
      buffer[addr_offset] = data_len;
      buffer[addr_offset + 1] = 0x46;
      //remaining buffer is data
      
      // add 3 to data_len for above bytes and send
      if (send(STK_PROG_PAGE, buffer, addr_offset - 1, data_len + 3, 0) == -1) {
        getDebugSerial()->println("Prog page failed");
        return -1;
      }
  
      uint8_t reply_len = send(STK_READ_PAGE, buffer, addr_offset - 1, 3, data_len);
      
      if (reply_len == -1) {
        getDebugSerial()->println("Read page failure");
        return -1;
      }
      
      if (reply_len != data_len) {
        getDebugSerial()->println("Read page length does not match");
        return -1;
      }
      
      // TODO we can compute checksum on buffer, reset and use for the read buffer!!!!!!!!!!!!!!!
      
      if (VERBOSE) {
        getDebugSerial()->print("Read page length is "); getDebugSerial()->println(reply_len, DEC);      
      }
      
      bool verified = true;
      
      // verify each byte written matches what is returned by bootloader
      for (int i = 0; i < reply_len; i++) {        
        if (read_buffer[i] != buffer[addr_offset + 2 + i]) {
          getDebugSerial()->print("Read page does not match write buffer at position "); getDebugSerial()->println(i, DEC);
          verified = false;
          break;
        }
      }
      
      if (!verified) {
        // retry is still attempts remaining
        if (z < PROG_PAGE_RETRIES) {
          getDebugSerial()->println("Failed to verify page.. retrying");
        } else {
          getDebugSerial()->println("Failed to verify page");
        }

        continue;
      }
      
      return 0;      
    }
    
  // read verify failure
  return -1;
}

void setup() {
  // can be slower but bootloader may timeout
  Serial.begin(19200);
  
  // leonardo wait for serial
  while (!Serial);
  
  pinMode(resetPin, OUTPUT);
  
  //diecimilao.upload.maximum_size=15872
  //diecimilao.upload.speed=115200
  // configure serial for bootloader baud rate  
  Serial1.begin(115200);
  
//  nss.begin(9600);
}

// send a noop command every MAX_OPTI_DELAY ms if we are reading a page of data or waiting for page data. this keeps optiboot from timing out
void check_noop() {
  if (prog_mode && bounced && millis() - last_optiboot_cmd > MAX_OPTI_DELAY) {
    getDebugSerial()->println("noop");
    if (send(STK_GET_SYNC, NULL, 0, 0, 0) == -1) {
      getDebugSerial()->println("noop fail");      
    }
    // force update timestamp regardless of outcome
    update_last_command();
  }
}

// called after each page is completed
void pageReset() {
  pos = 0;
  page_len = 0;
  is_first_page = false;
  is_last_page = false;  
}

// called after programming completes
void progReset() {
  pageReset();
  prog_mode = false;
  count = 0;
  bounced = false;
}


void loop() {
  
  uint8_t b = 0;

  // each page is ctrl,len,high,low,data
  // receive a page at a time, ex
  // f,80,e,94,9c,7,8,95,fc,1,16,82,17,82,10,86,11,86,12,86,13,86,14,82,34,96,bf,1,e,94,bd,7,8,95,dc,1,68,38,10,f0,68,58,29,c0,e6,2f,f0,e0,67,ff,13,c0,e0,58,f0,40,81,e0,90,e0,2,c0,88,f,99,1f
  while (getDebugSerial()->available() > 0) {
    b = getDebugSerial()->read();
    
    check_noop();
    
    if (pos == 0) {
      // don't need this is buffer
      buffer[pos] = b;
            
      if (b == FIRST_PAGE) {
        prog_mode = true;
        is_first_page = true;
      } else if (b == LAST_PAGE) {
        is_last_page = true;
      }
    } else if (pos == 1 && prog_mode) {
      // length is only the data length,  so add 4 byte (ctrl, len, addr high, low)
      buffer[pos] = b + 4;
      page_len = buffer[pos];      
      //getDebugSerial()->print("page len is "); getDebugSerial()->println(b, DEC);
    } else if (pos < page_len - 1 && prog_mode) {
      // data
      buffer[pos] = b;
    } else if (pos == (page_len - 1) && prog_mode) {
      // complete page
      buffer[pos] = b;
      
      if (is_first_page) {
        // first page, reset the target and perform check
        bounce();
        
        if (initTarget() != 0) {
          getDebugSerial()->println("Check failed!"); 
          progReset();
          continue;
        } 
        
        if (send(STK_ENTER_PROGMODE, buffer, 0, 0, 0) == -1) {
          getDebugSerial()->println("STK_ENTER_PROGMODE failure");
          progReset();
          continue;            
        }        
      }
      
      if (VERBOSE) {
        dump_buffer(buffer, "prog_page", 0, page_len);        
      }
      
      // substraact 4 since we don't send our header bytes to the bootloader
      if (send_page(2, page_len - 4) != -1) {
        // send ok after each page so client knows to send another
        getDebugSerial()->println("ok");       
        getDebugSerial()->flush();
      } else {
        getDebugSerial()->println("Send page failure"); 
        progReset();      
        continue;         
      }
      
      if (is_last_page) {
        // done. leave prog mode
        if (send(STK_LEAVE_PROGMODE, buffer, 0, 0, 0) == -1) {
          getDebugSerial()->println("STK_LEAVE_PROGMODE failure");
          progReset();
          continue;            
        }
        
        progReset();      
        continue; 
      } else {
        pageReset();  
        continue;      
      }
    }
     
    pos++;
    
    // TODO pos > 0 && !prog_mode
    if (pos >= BUFFER_SIZE) {
      getDebugSerial()->println("Error read past buffer");
      progReset();
    }
  }
  
  check_noop();
  
  // oops, got some data we are not expecting
  if (prog_mode && getProgrammerSerial()->available() > 0) {
    uint8_t ch = getProgrammerSerial()->read();
    getDebugSerial()->print("Unexpected reply @"); getDebugSerial()->print(count, DEC); getDebugSerial()->print(" "); getDebugSerial()->println(ch, HEX);
    count++;
  }
}
