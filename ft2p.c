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
#define MAX_MACRO_LEN   255  // real max is like 254?
#define MAX_OCTAVE      7
#define NUM_OCTAVES     7
#define NUM_SEMITONES   12
#define MAX_SFX         64
#define MAX_DECAY_START 15   // starting volume
#define MAX_DECAY_RATE  16   // decay rate
#define MAX_DECAY_LEN   256  // actually goes up to 224 or something but this is to be safe

//////////////////// constants ////////////////////
const char *scale = "cCdDefFgGaAb";
const char *supported_effects = "034GQRS";
const char *conductor_effects = "BCDF";
const char *chan_name[] = {"pulse1", "pulse2", "triangle", "noise", "drum", "attack"};

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

// types of envelopes
enum {
  MS_VOLUME,
  MS_ARPEGGIO,
  MS_PITCH,
  MS_HIPITCH,
  MS_DUTY,
  MACRO_SET_COUNT
};

// arpeggio types
enum {
  ARP_ABSOLUTE,
  ARP_RELATIVE,
  ARP_FIXED
};

// supported note effects
enum {
  FX_NONE     = 0, 
  FX_ARP      = '0', // arpeggio
  FX_SLUR     = '3', // enable slur if nonzero
  FX_VIBRATO  = '4', // vibrato change
  FX_LOOP     = 'B', // jump to frame X
  FX_FINE     = 'C', // stop song
  FX_PAT_CUT  = 'D', // skip to next pattern
  FX_TEMPO    = 'F', // change tempo or speed
  FX_DELAY    = 'G', // delay for X frames
  FX_SLUR_UP  = 'Q', // note for one row, slur into pitch X semitones up
  FX_SLUR_DN  = 'R', // note for one row, slur into pitch X semitones down
  FX_DELAYCUT = 'S', // grace note for X frames then rest
  FX_ATTACK_ON= 'J'  // repurposed to specify attack target
};

// a sound effect definition, held onto until 
typedef struct soundeffect {
  uint8_t instrument, channel;
  char name[64];
} soundeffect;

// a note on a pattern
typedef struct ftnote {
  uint8_t octave;             // octave number
  char note;                  // note name, capitalized if sharp
  char instrument;            // instrument number
  char effect[MAX_EFFECTS];   // effect name
  uint8_t param[MAX_EFFECTS]; // effect parameter
  uint8_t slur;               // nonzero if note has slur
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
  int pattern_length[MAX_PATTERNS][CHANNEL_COUNT];
  int effect_columns[CHANNEL_COUNT];
  int loop_to;

  // Song status information
  int pattern_id, frames;
} ftsong;

// an instument envelope
typedef struct ftmacro {
  uint8_t length, loop, release;
  char arp_type;
  char sequence[MAX_MACRO_LEN];
  uint8_t decay_rate;   // if 0, decay isn't used
  uint8_t decay_volume; // starting volume to use for decay
  uint8_t decay_index;  // index decay starts 
} ftmacro;

//////////////////// functions ////////////////////

ftnote make_note(uint8_t octave, char note, char instrument) {
  ftnote new_note;
  memset(&new_note, 0, sizeof(new_note));
  new_note.octave = octave;
  new_note.note = note;
  new_note.instrument = instrument;
  return new_note;
}

int note_to_semitone(char note, int octave) {
  return (strchr(scale, note)-scale)+(octave*NUM_SEMITONES);
}

void semitone_to_note(int semitone, char *note, uint8_t *octave) {
  // convert the number back to a note name and octave
  *note = scale[semitone % NUM_SEMITONES];
  *octave = semitone / NUM_SEMITONES;
}

// offsets a note by a given number of semitones
void shift_semitones(ftnote *note, int offset) {
  if(!isalpha(note->note))
    return;
  // convert to semitones so I can just shift the integer value
  int semitones = note_to_semitone(note->note, note->octave);
  // add to the integer value
  semitones += offset;
  // change back to a note
  semitone_to_note(semitones, &note->note, &note->octave);
}

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
    else if(*input == ' ' || *input == '-' || *input == '_') {
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
  // also set a pointer to the text after "start"
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
ftsong song, xsong;
int song_num = 0, sfx_num = 0;
char instrument[MAX_INSTRUMENTS][MACRO_SET_COUNT];
char instrument_used[MAX_INSTRUMENTS];
ftmacro instrument_macro[MACRO_SET_COUNT][MAX_INSTRUMENTS];
char instrument_name[MAX_INSTRUMENTS][32];
const char *in_filename, *out_filename;
char drum_name[NUM_OCTAVES][NUM_SEMITONES][16];
soundeffect soundeffects[MAX_SFX];
int decay_enabled = 0;
char decay_envelope[MAX_DECAY_START][MAX_DECAY_RATE][MAX_DECAY_LEN];

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

// writes an octave using ' and ,
void write_octave(FILE *file, int octave) {
  int i;
  if(octave > 2)
    for(i=2; i!= octave; i++)
      fputc('\'', file);
  if(octave < 2)
    for(i=2; i!= octave; i--)
      fputc(',', file);
}

// writes an instrument's envelopes
void write_instrument(FILE *file, int i, int absolute_pitch) {
  unsigned int num_macro_volume = (unsigned)instrument[i][MS_VOLUME];
  unsigned int num_macro_duty   = (unsigned)instrument[i][MS_DUTY];
  unsigned int num_macro_arp    = (unsigned)instrument[i][MS_ARPEGGIO];
  int decay_rate   = instrument_macro[MS_VOLUME][num_macro_volume].decay_rate;
  int decay_volume = instrument_macro[MS_VOLUME][num_macro_volume].decay_volume;
  int decay_index  = instrument_macro[MS_VOLUME][num_macro_volume].decay_index;

  if(instrument[i][MS_VOLUME] >= 0) {
    ftmacro macro = instrument_macro[MS_VOLUME][num_macro_volume];

    // do not use decay if it would interfere with the arpeggio or duty envelopes
    if(decay_rate && (instrument[i][MS_ARPEGGIO] < 0 || ((instrument_macro[MS_ARPEGGIO][num_macro_arp].length < decay_index) && 
                                                        (instrument_macro[MS_ARPEGGIO][num_macro_arp].loop == 255)))
                  && (instrument[i][MS_DUTY] < 0 || ((instrument_macro[MS_DUTY][num_macro_duty].length < decay_index) &&
                                                    (instrument_macro[MS_DUTY][num_macro_duty].loop == 255)))) {
      macro.sequence[decay_index] = decay_volume;
      macro.length = decay_index + 1;
      fprintf(file, "  decay %i\r\n", decay_rate);
    }
    fprintf(file, "  volume ");
    write_macro(file, &macro);
  }
  if(instrument[i][MS_DUTY] >= 0) {
    fprintf(file, "  timbre ");
    write_macro(file, &instrument_macro[MS_DUTY][num_macro_duty]);
  }
  if(instrument[i][MS_ARPEGGIO] >= 0) {
    ftmacro *macro = &instrument_macro[MS_ARPEGGIO][num_macro_arp];
    fprintf(file, "  pitch ");

    if(absolute_pitch) { // Pently sfx pitch envelopes require music notes, not semitone numbers
      int j;
      for(j=0; j<macro->length; j++) {
        if(j == macro->loop)
          fprintf(file, "| ");
        // convert to note
        int semitones = macro->sequence[j];
        char note;
        uint8_t octave;
        semitone_to_note(semitones, &note, &octave);
        // print it
        fprintf(file, "%c%s", tolower(note), isupper(note)?"#":"");
        write_octave(file, octave);
        fputc(' ', file);
      }
      fprintf(file, "\r\n");
    } else {
      write_macro(file, macro);
    }
  }
}

// converts the number of rows to a Pently note duration
void write_duration(FILE *file, int duration, int slur) {
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
  fprintf(file, "%s%s ", durations[duration%16], slur?"~":"");
  while(duration > 16) {
    fprintf(file, "w1 ");
    duration -= 16;
  }
}

// write a time in the format "at" takes
void write_time(FILE *file, int rows) {
  int measure = rows / 16;
  int beat    = (rows % 16) / 4;
  int row     = (rows % 16) % 4;

  fprintf(file, "%i", measure+1);
  if(beat || row) {
    fprintf(file, ":%i:%i", beat+1, row+1);
  }
}

// writes a tempo
void write_tempo(FILE *file, int speed, int tempo) {
  float real_tempo = 6;
  real_tempo /= speed;
  real_tempo *= tempo;
  fprintf(file, "  tempo %.2f", real_tempo);
}

// writes a pattern to the output file
void write_pattern(FILE *file, int id, int channel) {
  if(channel == CH_NOISE) // noise not implemented yet
    return;

  ftnote *pattern = xsong.pattern[id][channel];
  int i, slur = 0, delay_cut = 0;

  // find the instrument used for the pattern
  int instrument = -1;
  for(i=0; i<xsong.rows; i++)
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
  while(row < xsong.pattern_length[id][channel]) {
    char this_note = pattern[row].note;
    int next, octave = pattern[row].octave;

    // find the next note
    for(next = row+1; next < xsong.pattern_length[id][channel]; next++)
      if(pattern[next].note)
        break;
    // the distance between this note and the next note is the duration
    int duration = next-row;

    // write any instrument changes
    if(isalpha(this_note) && pattern[row].instrument >= 0 && pattern[row].instrument != instrument) {
      instrument = pattern[row].instrument;
      fprintf(file, " @%s ", instrument_name[instrument]);
    }

    // handle any effects
    for(i=0; i<MAX_EFFECTS; i++) {
      switch(pattern[row].effect[i]) {
        case FX_SLUR:
          slur = pattern[row].param[i] != 0;
          break;
        case FX_ARP:
          fprintf(file, "@EN%.2x ", pattern[row].param[i]);
          break;
        case FX_VIBRATO:
          fprintf(file, "@MP%x ", pattern[row].param[i] & 15);
          break;
        case FX_DELAY:
          fprintf(file, "r%ig ", pattern[row].param[i]);
          break;
        case FX_DELAYCUT:
          delay_cut = pattern[row].param[i];
          if(this_note && !pattern[row+1].note)
            pattern[row+1].note = '-';
          break;
      }
    }

    // write note
    if(this_note == '-' || !this_note) {
      fprintf(file, "r");
    } else if(channel != CH_DPCM) {
      fprintf(file, "%c%s", tolower(this_note), isupper(this_note)?"#":"");

      // shift the octave in the direction needed
      write_octave(file, octave);
    } else { // DPCM
      char *scale_note = strchr(scale, this_note);
      fprintf(file, "%s", drum_name[octave][scale_note-scale]);
    }
    if(delay_cut && isalpha(this_note)) {
      fprintf(file, "%ig r16 w", delay_cut);
      delay_cut = 0;
    }
    write_duration(file, duration, slur|pattern[row].slur);

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
  memset(&soundeffects, 0, sizeof(soundeffects));

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
  int need_song_export = 0;
  while(fgets(buffer, sizeof(buffer), input_file)) {
    char *arg;
    remove_line_endings(buffer);

    if(starts_with(buffer, "TRACK ", &arg)) {
      if(song_num) {
        need_song_export = 1;
        xsong = song;
      }
      song_num++;
      memset(&song, 0, sizeof(song));
      song.rows = strtol(arg, &arg, 10);
      for(i=0; i<MAX_PATTERNS; i++)
        for(j=0; j<CHANNEL_COUNT; j++)
          song.pattern_length[i][j] = song.rows;
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

         // skip if the note is already filled in
         if(song.pattern[song.pattern_id][channel][row].note)
           continue;

         // read note info
         ftnote note;
         memset(&note, 0, sizeof(note));
 
         note.instrument = -1;

         if(line[2] != '.') {
           // sharp note are uppercase
           note.note = (line[3]=='#')?toupper(line[2]):tolower(line[2]);
           // octave will be garbage for note cuts, but that's OK
           note.octave = line[4]-'0';

           // read instrument if it's there
           if(isalpha(note.note) && line[6] != '.') { // will have to change isalpha for eventual noise track support
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
         }

         // read effects
         for(j=0; j<song.effect_columns[channel]; j++) {
           // read in the effect type and value
           char *effect = line+11;
           note.effect[j] = *effect;
           note.param[j]  = strtol(effect+1, NULL, 16);

           // some effects call for processing during pattern reading
           ftnote *next_note = &song.pattern[song.pattern_id][channel][row+1];
           switch(*effect) {
             case FX_DELAYCUT: // fix for delaying a cut on an empty row
               if(!note.note)  // (which I don't think works yet??)
                 note.note = '~';
               break;
             case FX_SLUR:
               if(note.param[j]) // set slur on previous note
                 for(int k=row-1; k >= 0; k--)
                   if(song.pattern[song.pattern_id][channel][k].note) {
                     song.pattern[song.pattern_id][channel][k].slur = 1;
                     break;
                   }
               break;
             // mark the note as a slur and make the note to slur into
             case FX_SLUR_UP:
               note.slur = 1;
               *next_note = make_note(note.octave, note.note, note.instrument);
               shift_semitones(next_note, note.param[j]&15);
               break;
             case FX_SLUR_DN:
               note.slur = 1;
               *next_note = make_note(note.octave, note.note, note.instrument);
               shift_semitones(next_note, -(note.param[j]&15));
               break;
             // loops, pattern cuts and fines all reduce the length of the pattern
             case FX_LOOP:
               song.loop_to = note.param[j];
               goto pattern_cut;
             case FX_FINE:
               song.loop_to = -1;
             case FX_PAT_CUT:
             pattern_cut:
               song.pattern_length[song.pattern_id][channel] = row+1;
           }
         }

         // finally write the not we made into the pattern
         song.pattern[song.pattern_id][channel][row] = note;
      }

    }

    // comments are used for song metadata
    else if(starts_with(buffer, "COMMENT ", &arg)) {
      remove_line_ending(buffer, '\r');
      if(*arg == '\"')
        arg++;
      char *arg2;
      // import another file into this file
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
      // generate decay tables
      } else if(strcmp(arg, "auto decay")) {
        decay_enabled = 1;
        for(i=0;i<MAX_DECAY_START;i++)
          for(j=0;j<MAX_DECAY_RATE;j++) {
            int volume = (i+1)<<4;
            int value, index = 0, decay = j+1;
            
            while(volume >= 0x10) {
              volume -= decay;
              value = ((volume+8)>>4);
              decay_envelope[i][j][index++] = value;
            }
            if(value != 0)
              decay_envelope[i][j][index++] = 0;
          }
      // define a sound effect using an instrument
      } else if(starts_with(arg, "sfx ", &arg2)) {
        soundeffects[sfx_num].instrument = strtol(arg2, &arg2, 16);
        // skip to channel
        while(*arg2 == ' ')
          arg2++;
        // select the channel
        char channel = *(arg2++);
        if(channel == 's')
          channel = CH_SQUARE1;
        else if(channel == 'n')
          channel = CH_NOISE;
        else if(channel == 't')
          channel = CH_TRIANGLE;
        soundeffects[sfx_num].channel = channel;

        // skip to name
        while(*arg2 == ' ')
          arg2++;
        strlcpy(soundeffects[sfx_num].name, arg2, 64);
        sfx_num++;
      // define a drum using sound effects
      } else if(starts_with(arg, "drumsfx ", &arg2)) {
        fprintf(output_file, "drum %s\r\n", arg2);
      // drum = assign a drum to a DPCM note
      } else if(starts_with(arg, "drum ", &arg2)) {
        char *note = strchr(scale, tolower(arg2[0]));
        if(!note)
          die("invalid note in scale");
        char *octave_ptr = arg2+1;
        if(*octave_ptr == '#') 
          note++;
        if(!isdigit(*octave_ptr))
          octave_ptr++;
        int octave = *octave_ptr-'0';
        check_range("drum octave", octave, 0, NUM_OCTAVES);
        strlcpy(drum_name[octave][note-scale], octave_ptr+2, 16);
      }
    }

    else if(starts_with(buffer, "COLUMNS ", &arg)) {
      arg = skip_to_number(arg);
      for(i=0;*arg && (i < CHANNEL_COUNT);i++)
        song.effect_columns[i] = strtol(arg, &arg, 10);
    }

    else if(starts_with(buffer, "MACRO ", &arg)) {
      int setting = strtol(arg, &arg, 10);
      check_range("macro setting type", setting, 0, MACRO_SET_COUNT);
      int id = strtol(arg, &arg, 10);
      check_range("macro id", id, 0, MAX_INSTRUMENTS);
      instrument_macro[setting][id].loop = strtol(arg, &arg, 10);    // -1 gets set as 255
      instrument_macro[setting][id].release = strtol(arg, &arg, 10);
      instrument_macro[setting][id].length = 0;
      instrument_macro[setting][id].arp_type = strtol(arg, &arg, 10);
      arg = skip_to_number(arg);

      // read all the numbers and count them
      while(*arg) {
        instrument_macro[setting][id].sequence[instrument_macro[setting][id].length++] = strtol(arg, &arg, 10);
        if(instrument_macro[setting][id].length >= MAX_MACRO_LEN)
          die("instrument macro too long");
      }

      // if auto decay is enabled and this is a volume envelope, try to find a decay envelope
      if(decay_enabled && setting == MS_VOLUME && instrument_macro[setting][id].loop == 255 &&
        !instrument_macro[setting][id].sequence[instrument_macro[setting][id].length-1]) {

        int stop = 0;
        int length_envelope = instrument_macro[setting][id].length-1;     // length in bytes, including the zero so -1
        for(i=MAX_DECAY_START-1;i>=2 && !stop; i--)                       // try starting volumes in reverse order
          for(j=0; j<MAX_DECAY_RATE && !stop; j++) {
            int length_decay = strlen(decay_envelope[i][j]);              // length in bytes, not including zero

            int start_offset = length_envelope - length_decay;            // end of the envelope, backed up to where the decay would start
            if(!memcmp(instrument_macro[setting][id].sequence + start_offset, decay_envelope[i][j], length_decay)) {
              instrument_macro[setting][id].decay_index = start_offset;
              instrument_macro[setting][id].decay_volume = i+1;
              instrument_macro[setting][id].decay_rate = j+1;
              stop = 1;                                                   // break out of the loop
            }
          }
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
      xsong = song;
      // write sound effects
      for(i=0; i<sfx_num; i++) {
        int instrument = soundeffects[i].instrument, channel = soundeffects[i].channel;
        // sound effects don't like being put on "pulse1" so replace it with "pulse"
        const char *channel_name = chan_name[channel];
        if(channel == CH_SQUARE1)
          channel_name = "pulse";
        fprintf(output_file, "\r\nsfx %s on %s\r\n", soundeffects[i].name, channel_name);
        write_instrument(output_file, instrument, channel != CH_NOISE);
      }
      // write instruments
      for(i=0; i<MAX_INSTRUMENTS; i++)
        if(instrument_used[i]) {
          fprintf(output_file, "\r\ninstrument %s\r\n", instrument_name[i]);
          write_instrument(output_file, i, 0);
        }
      need_song_export = 1;
    }
    if(need_song_export) {
      fprintf(output_file, "\r\nsong %s\r\n  time 4/4\r\n  scale 16\r\n", xsong.name);
      write_tempo(output_file, xsong.speed, xsong.tempo);
      fprintf(output_file, "\r\n");

      // write the actually used (not empty) patterns
      for(j=0; j<CHANNEL_COUNT; j++)
        for(i=0; i<MAX_PATTERNS; i++) {
          int not_empty = 0;
          for(int row = 0; row < xsong.rows; row++)
            if(isalpha(xsong.pattern[i][j][row].note)) {
              not_empty = 1;
              break;
            }
          xsong.pattern_used[i][j] = not_empty;

          if(not_empty)
            write_pattern(output_file, i, j);
        }

      // write the frames
      int channel_playing[CHANNEL_COUNT] = {0, 0, 0, 0, 0, 0};
      int total_rows = 0;
      for(i=0; i<xsong.frames; i++) {
        fprintf(output_file, "\r\n  at ");;
        write_time(output_file, total_rows);
        if(xsong.loop_to == i && xsong.loop_to)
          fprintf(output_file, "\r\n  segno");

        int min_length = MAX_ROWS; // minimum pattern length in this frame
        for(j=0; j<CHANNEL_COUNT; j++) {
          int pattern = xsong.frame[i][j];
          if(j != CH_NOISE && xsong.pattern_used[pattern][j]) {
            fprintf(output_file, "\r\n  play pat_%i_%i_%i", song_num, j, pattern);
            channel_playing[j] = 1;
          } else if(channel_playing[j]) { // stop channel if it was playing but now it isn't
            fprintf(output_file, "\r\n  stop %s", chan_name[j]);
            channel_playing[j] = 0;
          }
          if(xsong.pattern_length[pattern][j] < min_length)
            min_length = xsong.pattern_length[pattern][j];
        }

        // look for tempo changes
        for(int row=0; row<min_length; row++) {
          int speed = 0, tempo = 0, attack=-1;
          for(int j=0; j<CHANNEL_COUNT; j++) {
            int pattern = xsong.frame[i][j];
            ftnote *note = &xsong.pattern[pattern][j][row];
            for(int fx=0; fx<MAX_EFFECTS; fx++)
              if(note->effect[fx] == FX_TEMPO) {
                if(note->param[fx] < 0x20)
                  speed = note->param[fx];
                else
                  tempo = note->param[fx];
              } else if(note->effect[fx] == FX_ATTACK_ON)
                attack = note->param[fx];
          }
          if(speed||tempo||(attack>=0)) {
            if(row) {
              fprintf(output_file, "\r\n  at ");
              write_time(output_file, total_rows+row);
            }
            if(speed||tempo) {
              fprintf(output_file, "\r\n");
              write_tempo(output_file, speed?speed:xsong.speed, tempo?tempo:xsong.tempo);
            }
            if(attack>=0) {
              fprintf(output_file, "\r\n  attack on %s", chan_name[attack]);
            }
          }
        }
        total_rows += min_length;
      }
      fprintf(output_file, "\r\n  at ");
      write_time(output_file, total_rows);
      fprintf(output_file, "\r\n  ");
      if(xsong.loop_to != -1)
        fprintf(output_file, "dal segno");
      else
        fprintf(output_file, "fine");

      need_song_export = 0;
    }

  }

  // close files
  fclose(input_file);
  fprintf(output_file, "\r\n\r\n");
  fclose(output_file);

  return 0;
}
