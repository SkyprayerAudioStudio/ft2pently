/*
 * ft2pently
 *
 * Copyright (C) 2016 NovaSquirrel
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
// https://github.com/Qix-/pently/issues/4
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>

// max values for some things
#define MAX_EFFECTS     4
#define MAX_ROWS        256
#define MAX_FRAMES      32   // real max is 128
#define MAX_PATTERNS    32   // real max is 128
#define MAX_INSTRUMENTS 64   // pently limit is 256/5
#define MAX_MACRO_LEN   128  // real max is like, 254?
#define MAX_OCTAVE      7
#define NUM_OCTAVES     7
#define NUM_SEMITONES   12

//////////////////// constants ////////////////////
const char *scale = "cCdDefFgGaAb";
const char *supported_effects = "03BCDGQRS";
const char *chan_name[] = {"pulse1", "pulse2", "triangle", "noise", "", "attack"};

//////////////////// enums and structs ////////////////////
enum {
  CH_SQUARE1,
  CH_SQUARE2,
  CH_TRIANGLE,
  CH_NOISE,
  CH_DPCM,
  CH_ATTACK,
  CHANNEL_COUNT
};

// 
enum {
  MS_VOLUME,
  MS_ARPEGGIO,
  MS_PITCH,
  MS_HIPITCH,
  MS_DUTY,
  MACRO_SET_COUNT
};

// supported note effects
enum {
  FX_NONE     = 0, 
  FX_ARP      = '0', // arpeggio
  FX_SLUR     = '3', // enable slur if nonzero
  FX_LOOP     = 'B', // jump to frame X
  FX_FINE     = 'C', // stop song
  FX_PAT_CUT  = 'D', // skip to next pattern
  FX_DELAY    = 'G', // delay for X frames
  FX_SLUR_UP  = 'Q', // note for one row, slur into pitch X semitones up
  FX_SLUR_DN  = 'R', // note for one row, slur into pitch X semitones down
  FX_DELAYCUT = 'S'  // grace note for X frames then rest
};

// a note on a pattern
typedef struct ftnote {
  uint8_t octave;             // octave number
  char note;                  // note name, capitalized if sharp
  char instrument;            // instrument number
  char effect[MAX_EFFECTS];   // effect name
  uint8_t param[MAX_EFFECTS]; // effect parameter
} ftnote;

// a song and its patterns
typedef struct ftsong {
  // Explicitly stated song information
  char name[32];
  int rows, speed, tempo;

  // Buffers to hold song information
  int frame[MAX_FRAMES][CHANNEL_COUNT];
  ftnote pattern[MAX_PATTERNS][CHANNEL_COUNT][MAX_ROWS];
  uint8_t pattern_used[MAX_PATTERNS][CHANNEL_COUNT];

  // Song status information
  int pattern_id, frames;
} ftsong;

// an instument envelope
typedef struct ftmacro {
  uint8_t length, loop, release;
  char sequence[MAX_MACRO_LEN];
} ftmacro;

//////////////////// functions ////////////////////

// shows an error and stops the program
void die(const char *reason) {
  puts(reason);
  exit(-1);
}

// asserts that a value is in a given range
void check_range(const char *name, int value, int low, int high) {
  if(value >= low && value < high)
    return;
  printf("Error: %s out of range (%i, must be below %i)\n", name, value, high);
  exit(-1);
}

// like strncpy but good
void strlcpy(char *Destination, const char *Source, int MaxLength) {
  // MaxLength is directly from sizeof() so it includes the zero
  int SourceLen = strlen(Source);
  if((SourceLen+1) < MaxLength)
    MaxLength = SourceLen + 1;
  memcpy(Destination, Source, MaxLength-1);
  Destination[MaxLength-1] = 0;
}

// removes one line ending if found
void remove_line_ending(char *text, char ending) {
  text = strrchr(text, ending);
  if(text)
    *text = 0;
}

// removes \n, \r or " if found on the end of a string
void remove_line_endings(char *buffer) {
  remove_line_ending(buffer, '\n');
  remove_line_ending(buffer, '\r');
  remove_line_ending(buffer, '\"');
}

// makes a label-friendly version of a name
char *sanitize_name(char *outbuf, const char *input, int length) {
  char hex[3];
  char temp[strlen(input)*2+1];
  char *output = temp;

  if(!isalpha(*input) && *input != '_') // names usually have to start with an letter
    *(output++) = '_';
  while(*input) {
    if(isalnum(*input))    // copy directly if alphanumeric
      *(output++) = *input;
    else if(*input == ' ' || *input == '-') {
      *(output++) = '_';
    } else {               // escape it otherwise
      sprintf(hex, "%.2x", *input);
      strcpy(output, hex);
      output += 2;
    }
    input++;
  }
  *(output) = 0;

  strlcpy(outbuf, temp, length);
  return outbuf;
}

// return 1 iff a string starts with another specific string
int starts_with(char *string, const char *start, char **arg) {
  if(arg)
    *arg = string+strlen(start);
  return !memcmp(string, start, strlen(start));
}

// increases a pointer until it gets to a digit or a dash
char *skip_to_number(char *string) {
  while(*string && (!isdigit(*string) || *string=='-'))
    string++;
  return string;
}

//////////////////// global variables ////////////////////
ftsong song;
int song_num = 0;
char instrument[MAX_INSTRUMENTS][MACRO_SET_COUNT];
char instrument_used[MAX_INSTRUMENTS];
ftmacro instrument_macro[MACRO_SET_COUNT][MAX_INSTRUMENTS];
char instrument_name[MAX_INSTRUMENTS][32];
const char *in_filename, *out_filename;
char drum_name[NUM_OCTAVES][NUM_SEMITONES][16];

// writes the numbers for an instrument's envelope, including the loop point
void write_macro(FILE *file, ftmacro *macro) {
  int i;
  for(i=0; i<macro->length; i++) {
    if(i == macro->loop)
      fprintf(file, "| ");
    fprintf(file, "%i ", macro->sequence[i]);
  }
  fprintf(file, "\r\n");
}

// converts the number of rows to a Pently note duration
void write_duration(FILE *file, int duration) {
  const char *durations[] = {
    /* 1 */ "16",
    /* 2 */ "8",
    /* 3 */ "8 w16",
    /* 4 */ "4",
    /* 5 */ "4 w16",
    /* 6 */ "4 w8",
    /* 7 */ "4 w8 w16",
    /* 8 */ "2",
    /* 9 */ "2 w16",
    /*10 */ "2 w8",
    /*11 */ "2 w8 w16",
    /*12 */ "2 w4",
    /*13 */ "2 w4 w16",
    /*14 */ "2 w4 w8",
    /*15 */ "2 w4 w8 w16",
    /*16 */ "1"
  };
  duration--;
  fprintf(file, "%s ", durations[duration%16]);
  while(duration > 16) {
    fprintf(file, "w1 ");
    duration -= 16;
  }
}

// writes a pattern to the output file
void write_pattern(FILE *file, int id, int channel) {
  if(channel == CH_NOISE) // noise not implemented yet
    return;

  ftnote *pattern = song.pattern[id][channel];
  int i;

  // find the instrument used for the pattern
  int instrument = -1;
  for(i=0; i<song.rows; i++)
    if(pattern[i].instrument >= 0) {
      instrument = pattern[i].instrument;
      break;
    }
  // generate pattern name and specify absolute octaves
  fprintf(file, "\r\n  pattern pat_%i_%i_%i", song_num, channel, id);
  if(channel != CH_DPCM && channel != CH_NOISE)
    fprintf(file, " with %s on %s\r\n    absolute", instrument_name[instrument], chan_name[channel]);
  fprintf(file, "\r\n    ");

  int row = 0;
  while(row < song.rows) {
    char this_note = pattern[row].note;
    int next, octave = pattern[row].octave;

    // find the next note
    for(next = row+1; next < song.rows; next++)
      if(pattern[next].note)
        break;
    // the distance between this note and the next note is the duration
    int duration = next-row;

    // write any instrument changes
    if(pattern[row].instrument != instrument) {
      instrument = pattern[row].instrument;
      fprintf(file, " @%s ", instrument_name[instrument]);
    }

    // write note
    if(this_note == '-' || !this_note)
      fprintf(file, "r");
    else if(channel != CH_DPCM) {
      fprintf(file, "%c%s", tolower(this_note), isupper(this_note)?"#":"");

      // shift the octave in the direction needed
      if(octave > 2)
        for(i=2; i!= octave; i++)
          fputc('\'', file);
      if(octave < 2)
        for(i=2; i!= octave; i--)
          fputc(',', file);
    } else { // DPCM
      char *scale_note = strchr(scale, this_note);
      fprintf(file, "%s", drum_name[octave][scale_note-scale]);
    }
    write_duration(file, duration);

    row = next;
  }

}

int main(int argc, char *argv[]) {
  int i, j;
  char buffer[700];
  memset(&instrument, 0, sizeof(instrument));
  memset(&instrument_used, 0, sizeof(instrument_used));
  memset(&instrument_macro, 0, sizeof(instrument_macro));
  memset(&instrument_name, 0, sizeof(instrument_name));
  memset(&drum_name, 0, sizeof(drum_name));

  // read arguments
  for(i=1; i<argc; i++) {
    if(!strcmp(argv[i], "-i"))
      in_filename = argv[i+1];
    if(!strcmp(argv[i], "-o"))
      out_filename = argv[i+1];
  }

  // complain if input or output not specified
  if(!in_filename || !out_filename)
    die("syntax: ft2p -i input -o output");

  // start reading file
  FILE *input_file = fopen(in_filename, "rb");
  if(!input_file)
    die("Input file couldn't be opened");
  FILE *output_file = fopen(out_filename, "wb");
  if(!output_file)
    die("Output file couldn't be opened");
  fprintf(output_file, "durations stick\r\nnotenames english\r\n");

  // process each line
  int need_song_export = -1, need_instrument_export = 1;
  while(fgets(buffer, sizeof(buffer), input_file)) {
    char *arg;
    remove_line_endings(buffer);

    if(starts_with(buffer, "TRACK ", &arg)) {
      need_song_export++;
      song_num++;
      memset(&song, 0, sizeof(song));
      song.rows = strtol(arg, &arg, 10);
      song.speed = strtol(arg, &arg, 10);
      song.tempo = strtol(arg, &arg, 10);
      arg = strchr(arg, '\"');
      sanitize_name(song.name, arg+1, sizeof(song.name));
    }

    else if(starts_with(buffer, "PATTERN ", &arg)) {
      song.pattern_id = strtol(arg, NULL, 16);
      check_range("pattern id", song.pattern_id, 0, MAX_PATTERNS);
    }

    else if(starts_with(buffer, "ROW ", &arg)) {
      int row = strtol(arg, &arg, 16);
      check_range("row id", row, 0, MAX_ROWS);

      for(int channel=0; channel<CHANNEL_COUNT; channel++) {
         // find next channel
         arg = strchr(arg, ':');
         if(!arg)
           break;
         char *line = arg;
         arg++;

         // read note info
         ftnote note;
         memset(&note, 0, sizeof(note));
 
         note.instrument = -1;

         if(line[2] == '.') // if no note, skip
           continue;

         // sharp note are uppercase
         note.note = (line[3]=='#')?toupper(line[2]):tolower(line[2]);
         // octave will be garbage for note cut
         note.octave = line[4]-'0';

         // read instrument if it's there
         if(line[6] != '.') {
           note.instrument = strtol(line+6, NULL, 16);
           check_range("instrument id", note.instrument, 0, MAX_INSTRUMENTS);
           instrument_used[(unsigned)note.instrument] = 1;
         } else { // if it's not, go back and find it
           for(j=row-1; j>=0; j--)
             if(song.pattern[song.pattern_id][channel][j].note && (song.pattern[song.pattern_id][channel][j].instrument != -1)) {
               note.instrument = song.pattern[song.pattern_id][channel][j].instrument;
               break;
             }
         }

         if(line[9] != '.')
           die("volume column not supported");

         song.pattern[song.pattern_id][channel][row] = note;

         // to do: effects
         
      }

    }
    else if(starts_with(buffer, "COMMENT ", &arg)) {
      remove_line_ending(buffer, '\r');
      if(*arg == '\"')
        arg++;
      char *arg2;
      if(starts_with(arg, "include ", &arg2)) {
        FILE *included = fopen(arg2, "rb");
        if(!included)
          die("couldn't open included file");
        while(!feof(included)) {
          char c = fgetc(included);
          if(c != EOF)
            fputc(c, output_file);
        }
        fclose(included);
      } else if(starts_with(arg, "drum ", &arg2)) {
        char *note = strchr(scale, arg2[0]);
        if(!note)
          die("invalid note in scale");
        int octave = arg2[1]-'0';
        check_range("drum octave", octave, 0, NUM_OCTAVES);
        strlcpy(drum_name[octave][note-scale], arg2+3, 16);
      }
    }

    else if(starts_with(buffer, "MACRO ", &arg)) {
      int setting = strtol(arg, &arg, 10);
      check_range("macro setting type", setting, 0, MACRO_SET_COUNT);
      int id = strtol(arg, &arg, 10);
      check_range("macro id", id, 0, MAX_INSTRUMENTS);
      instrument_macro[setting][id].loop = strtol(arg, &arg, 10);
      instrument_macro[setting][id].release = strtol(arg, &arg, 10);
      instrument_macro[setting][id].length = 0;
      strtol(arg, &arg, 10); // skip unknown number
      arg = skip_to_number(arg);

      // read all the numbers and count them
      while(*arg) {
        instrument_macro[setting][id].sequence[instrument_macro[setting][id].length++] = strtol(arg, &arg, 10);
        if(instrument_macro[setting][id].length >= MAX_MACRO_LEN)
          die("instrument macro too long");
      }
    }

    else if(starts_with(buffer, "INST2A03 ", &arg)) {
      int id = strtol(arg, &arg, 10);
      check_range("instrument id", song.pattern_id, 0, MAX_INSTRUMENTS);
      for(i=0; i<MACRO_SET_COUNT; i++) {
        instrument[id][i] = strtol(arg, &arg, 10);
        check_range("macro sequence id", instrument[id][i], -1, MAX_INSTRUMENTS);
      }
      arg = strchr(arg, '\"');
      sanitize_name(instrument_name[id], arg+1, sizeof(instrument_name[id]));
    }

    else if(starts_with(buffer, "ORDER ", &arg)) {
      int id = strtol(arg, &arg, 16);
      song.frames = id+1; // assume last frame in file is last frame in song
      check_range("frame number", id, 0, MAX_FRAMES);
      arg = skip_to_number(arg);
      for(i=0; i<CHANNEL_COUNT; i++)
        song.frame[id][i] = strtol(arg, &arg, 16);
    }

    // export things if needed
    if(!strcmp(buffer, "# End of export")) {
      if(need_instrument_export) {
        need_instrument_export = 0;
        for(i=0; i<MAX_INSTRUMENTS; i++)
          if(instrument_used[i]) {
            fprintf(output_file, "\r\ninstrument %s\r\n", instrument_name[i]);
            if(instrument[i][MS_VOLUME] >= 0) {
              fprintf(output_file, "  volume ");
              write_macro(output_file, &instrument_macro[MS_VOLUME][(unsigned)instrument[i][MS_VOLUME]]);
            }
            if(instrument[i][MS_DUTY] >= 0) {
              fprintf(output_file, "  timbre ");
              write_macro(output_file, &instrument_macro[MS_DUTY][(unsigned)instrument[i][MS_DUTY]]);
            }
            if(instrument[i][MS_ARPEGGIO] >= 0) {
              fprintf(output_file, "  pitch ");
              write_macro(output_file, &instrument_macro[MS_ARPEGGIO][(unsigned)instrument[i][MS_ARPEGGIO]]);
            }
          }
      }
      need_song_export = 1;
    }
    if(need_song_export == 1) {
      fprintf(output_file, "\r\nsong %s\r\n", song.name);
      float real_tempo = 6;
      real_tempo /= song.speed;
      real_tempo *= song.tempo;
      fprintf(output_file, "  time 4/4\r\n  scale 16\r\n  tempo %.2f\r\n", real_tempo);

      // write the actually used (not empty) patterns
      for(j=0; j<CHANNEL_COUNT; j++)
        for(i=0; i<MAX_PATTERNS; i++) {
          int not_empty = 0;
          for(int row = 0; row < song.rows; row++)
            if(isalpha(song.pattern[i][j][row].note)) {
              not_empty = 1;
              break;
            }
          song.pattern_used[i][j] = not_empty;

          if(not_empty)
            write_pattern(output_file, i, j);
        }

      // write the frames
      int channel_playing[CHANNEL_COUNT] = {0, 0, 0, 0, 0, 0};
      for(i=0; i<song.frames; i++) {
        fprintf(output_file, "\r\n  at %i", 1+(song.rows/16)*i);
        for(j=0; j<CHANNEL_COUNT; j++) {
          int pattern = song.frame[i][j];
          if(j != CH_NOISE && j != CH_ATTACK && song.pattern_used[pattern][j]) {
            fprintf(output_file, "\r\n  play pat_%i_%i_%i", song_num, j, pattern);
            channel_playing[j] = 1;
          } else if(channel_playing[j]) {
            fprintf(output_file, "\r\n  stop %s", chan_name[j]);
            channel_playing[j] = 0;
          }
        }
      }
      fprintf(output_file, "\r\n  at %i\r\n  fine", 1+(song.rows/16)*song.frames);

      need_song_export = 0;
    }

  }

  // close files
  fclose(input_file);
  fprintf(output_file, "\r\n\r\n");
  fclose(output_file);

  return 0;
}