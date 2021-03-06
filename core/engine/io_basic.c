/*
 *  io_basic.c
 *
 *  Input/output predicates (see engine(io_basic)).
 *
 *  Copyright (C) 1996,1997,1998, 1999, 2000, 2001, 2002 UPM-CLIP
 */

/* TODO: This code should be generic for any stream */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>  /* for atoi MCL */
#include <string.h>
#include <strings.h>

#include <ciao/os_signal.h>
#include <ciao/threads.h>
#include <ciao/datadefs.h>
#include <ciao/support.h>
#include <ciao/support_macros.h>
#include <ciao/task_areas.h>
#include <ciao/misc.h>
#include <ciao/interrupt.h>

#include <ciao/rune.h>
#include <ciao/term_support.h>
#include <ciao/io_basic.h>
#include <ciao/stream_basic.h>
#include <ciao/tasks.h>
#include <ciao/start.h>
#include <ciao/alloc.h>
#include <ciao/bignum.h>
#include <ciao/stacks.h>

/* local declarations */

/* Own version of getc() that normalizes EOF (<0) to -1 */
#if (EOF == -1)
static inline int c_getc(FILE *f) {
  return getc(f);
}
#else
static inline int c_getc(FILE *f) {
  int i = getc(f);
  return (i < 0 ? -1 : i);
}
#endif

#define BYTE_EOF       (-1)
#define BYTE_PAST_EOF  (-2)

static CFUN__PROTO(readrune, c_rune_t, stream_node_t *s, int op_type, definition_t *pred_address);
static CVOID__PROTO(writerune, c_rune_t r, stream_node_t *s);
static CVOID__PROTO(writerunen, c_rune_t r, int i, stream_node_t *s);
static CFUN__PROTO(readbyte, int, stream_node_t *s, int op_type, definition_t *pred_address);
static CVOID__PROTO(writebyte, int ch, stream_node_t *s);

#if defined(USE_MULTIBYTES)
c_rune_t getmb(FILE * f);
c_rune_t putmb(c_rune_t c, FILE * f);
int readmb(int fildes, c_rune_t * c);
int writemb(int fildes, c_rune_t c);

#define BIT1 7
#define BITX 6
#define BIT2 5
#define BIT3 4
#define BIT4 3

#define BYTE1 ((1 << BIT1) ^ 0xff)             // 0111 1111
#define BYTEX ((1 << BITX) ^ 0xff)             // 1011 1111
#define BYTE2 ((1 << BIT2) ^ 0xff)             // 1101 1111
#define BYTE3 ((1 << BIT3) ^ 0xff)             // 1110 1111
#define BYTE4 ((1 << BIT4) ^ 0xff)             // 1111 0111

#define TAG1  (((1 << (BIT1 + 1)) - 1) ^ 0xff) // 0000 0000
#define TAGX  (((1 << (BITX + 1)) - 1) ^ 0xff) // 1000 0000
#define TAG2  (((1 << (BIT2 + 1)) - 1) ^ 0xff) // 1100 0000
#define TAG3  (((1 << (BIT3 + 1)) - 1) ^ 0xff) // 1110 0000
#define TAG4  (((1 << (BIT4 + 1)) - 1) ^ 0xff) // 1111 0000

#define MASKX ((1 << BITX) - 1)                // 0011 1111

#define RUNE1 ((1 << (BIT1 + 0*BITX)) - 1)     // 0000 0000 0000 0000 0111 1111
#define RUNE2 ((1 << (BIT2 + 1*BITX)) - 1)     // 0000 0000 0000 0111 1111 1111
#define RUNE3 ((1 << (BIT3 + 2*BITX)) - 1)     // 0000 0000 1111 1111 1111 1111
#define RUNE4 ((1 << (BIT4 + 3*BITX)) - 1)     // 0001 1111 1111 1111 1111 1111 

#endif // USE_MULTIBYTES

CVOID__PROTO(display_term, tagged_t term, stream_node_t *stream, bool_t quoted);

#define CheckGetRune(X,C,ArgNo) ({                                      \
  if (TaggedIsSmall((X))) {                                             \
    C = GetSmall((X));                                                  \
  } else if (TagIsLarge((X)) && !LargeIsFloat((X))) { /* bigint */      \
    BUILTIN_ERROR(REPRESENTATION_ERROR(CHARACTER_CODE), (X), (ArgNo));  \
  } else {                                                              \
    ERROR_IN_ARG((X), (ArgNo), INTEGER);                                \
  }                                                                     \
  if (!isValidRune((C))) {                                              \
    BUILTIN_ERROR(REPRESENTATION_ERROR(CHARACTER_CODE), (X), (ArgNo));  \
  }                                                                     \
})

#define CheckGetByte(X,C,ArgNo) ({               \
  if (!TaggedIsSmall((X))) {                     \
    ERROR_IN_ARG((X), (ArgNo), (TY_BYTE));       \
  }                                              \
  C = GetSmall((X));                             \
  if ((C) < 0 || (C) > 255) {                    \
    ERROR_IN_ARG((X), (ArgNo), (TY_BYTE));       \
  } \
})

/* TODO: throw better exception */
#define IO_ERROR(Message) ({                    \
  perror((Message));                          \
  UNLOCATED_EXCEPTION(RESOURCE_ERROR(R_UNDEFINED)); \
})

/* UTF8 Support */
#if defined(USE_MULTIBYTES)

int c_mbtorune(c_rune_t *pr, const char *s) {
  uint32_t s0, sx;
  c_rune_t c;
    
  s0 = (unsigned char) s[0];
  
  c = s0;
  
  if (s0 <= BYTE1) {    // 0 <= s[0] <= BYTE1, i.e. 1 byte sequence 
    *pr = c;
    return 1;
  }
  if (s0 <= BYTEX) {    // BYTE1 < s[0] <= BYTEX, i.e. improper first byte
    goto bad;
  }

  // At least 2 bytes sequence

  sx = ((unsigned char) s[1]);
  if (sx <= BYTE1 || BYTEX < sx) {  // !(BYTE1 < s[1] <= BYTEX), i.e. improper second byte
    goto bad;
  }
  c = (c << BITX) | (sx & MASKX);
  
  if (s0 <= BYTE2) {    // BYTEX < s[0] <= BYTE2, i.e. 2 bytes sequence 
    c = c & RUNE2;
    if (c <= RUNE1) {   // overlong sequence, i.e. c should be encoded using 1 byte
      goto bad;
    }
    *pr = c;
    return 2;
  }
  
  // At least 3 bytes sequence
  
  sx = ((unsigned char) s[2]);
  if (sx <= BYTE1 || BYTEX < sx) {  // !(BYTE1 < s[2] <= BYTEX), i.e. improper third byte
    goto bad;
  }
  c = (c << BITX) | (sx & MASKX);
    
  if (s0 <= BYTE3) {    // BYTE2 < s[0] <= BYTE3, i.e. 3 bytes sequence
    c = c & RUNE3;
    if (c <= RUNE2) {   // overlong sequence, c should be encoded using 2 bytes or less
      goto bad;
    }
    if (RUNE_SURROGATE_MIN <= c && c <= RUNE_SURROGATE_MAX) { // c is an invalid rune
      goto bad;
    }
    *pr = c;
    return 3;
  }
  
  // 4 bytes sequence

  sx = ((unsigned char) s[3]);
  if (sx <= BYTE1 || BYTEX < sx) {  // !(BYTE1 < s[3] <= BYTEX), i.e. improper fourth byte
    goto bad;
  }

  c = (c << BITX) | (sx & MASKX);

  if (s0 <= BYTE4) {    // BYTE3 < s[0] <= BYTE4, i.e. 4 bytes sequence
    c = c & RUNE4;
    if (c <= RUNE3) {   // overlong sequence, c should be encoded using 3 bytes or less
      goto bad;
    }
    if (RUNE_MAX < c) { // c is an invalide rune
      goto  bad;
    }
    *pr = c;
    return 4;
  }

  // BYTE4 < s[0], i.e. improper first byte
      
 bad:
  *pr = RUNE_ERROR;
  return 1;
}

int c_runetomb(char * s, c_rune_t rune) {
  uint32_t c = (uint32_t) rune;

  if (c <= RUNE1) { 
    // rune encodes to 1 byte
    *s =  TAG1 | c;
    return 1;
  }
  
  if (c <= RUNE2) {
    // rune encodes to 2 bytes
    *(s++) = TAG2 |  (c >> 1*BITX);
    *s     = TAGX | ((c >> 0*BITX) & MASKX);
    return 2;
  } 
  
  // Do this test here because RUNE_ERROR uses 3 bytes
  if (!isValidRune(c)) {
    c = (uint32_t) RUNE_ERROR;
  }
  
  if (c <= RUNE3) {
    // rune encodes to 3 bytes
    *(s++) = TAG3 |  (c >> 2*BITX);
    *(s++) = TAGX | ((c >> 1*BITX) & MASKX);
    *s     = TAGX | ((c >> 0*BITX) & MASKX);
    return 3;
  }
  
  // rune encodes to 4 bytes
  *(s++) = TAG4 |  (c >> 3*BITX);
  *(s++) = TAGX | ((c >> 2*BITX) & MASKX);
  *(s++) = TAGX | ((c >> 1*BITX) & MASKX);
  *s     = TAGX | ((c >> 0*BITX) & MASKX);
  return 4;
}

int c_mblen(const char *s) {
  uint32_t c = (unsigned char) s[0];

  if (c <= BYTE1) return 1;
  if (c <= BYTEX) return -1;
  if (c <= BYTE2) return 2;
  if (c <= BYTE3) return 3;
  if (c <= BYTE4) return 4;
  return -1;
}

int c_mbstrlen(const char * s) {
  int i = 0;

  while(*s) {
    s += c_mblen(s);
    i++;
  }

  return i;
}

c_rune_t getmb(FILE * f) {
  char buff[C_MB_LEN_MAX];
  c_rune_t c;
  int i, n;
  
  c = c_getc(f);
  if (c < 0) {   // IO error or EOF
    return RUNE_EOF;
  }

  buff[0] = c;
  n = c_mblen(buff);
  if (n < 0) {   // Improper first byte
    return RUNE_ERROR;
  }
  
  for (i = 1; i < n; i++) {
    c = c_getc(f);
    if (c <= BYTE1 || BYTEX < c) { // IO error or EOF or improper following byte
      return RUNE_ERROR;
    }
    buff[i] = (char) c;
  }

  c_mbtorune(&c, buff);
  return c;
}

c_rune_t putmb(c_rune_t c, FILE * f) {
  char buff[C_MB_LEN_MAX];
  int n;
  
  n = c_runetomb(buff, c);
  
  for (int i=0; i<n; i++) {
    if (putc(buff[i], f) < 0) {
      return RUNE_EOF;
    }
  }
  
  return c;
}

int readmb(int fildes, c_rune_t *c) {
  char buff[C_MB_LEN_MAX];
  int d, i, m, n;
  
  m = read(fildes, buff, 1);
  if (m <= 0) { // IO error or EOF
    return m;
  }

  n = c_mblen(buff);
  if (n < 0) {
    *c = RUNE_ERROR;
    return 1;
  }
  
  for (i = 1; i < n; i++) {
    m = read(fildes, buff+i, 1);
    d = (unsigned char) buff[i];
    if (m <= 0 || d <= BYTE1 || BYTEX < d) {
      // IO error or EOF or improper following byte
      *c = RUNE_ERROR;
      return i;
    }
  }
  
  c_mbtorune(c, (char*) buff);
  return i;  
}

int writemb(int fildes, c_rune_t c) {
  char buff[C_MB_LEN_MAX];
  int n;

  n = c_runetomb(buff, c);
  return write(fildes, buff, n);  
}

#endif // defined(USE_MULTIBYTES)

CBOOL__PROTO(code_class) {
  ERR__FUNCTOR("io_basic:code_class", 2);
  c_rune_t i;

  DEREF(X(0), X(0));
  CheckGetRune(X(0),i,1);
  //  if (!TaggedIsSmall(X(0))) return FALSE;
  //  i = GetSmall(X(0));                             

  return cunify(Arg,X(1),MakeSmall(get_rune_class(i)));
}

static inline void inc_counts(int ch, stream_node_t * stream) {
  stream->rune_count++;
  if (ch == 0xd) {
    stream->last_nl_pos = stream->rune_count;
    stream->nl_count++;
  } else if (ch == 0xa) {
    stream->last_nl_pos = stream->rune_count;
    if (stream->previous_rune != 0xd)
      stream->nl_count++;
  }
  stream->previous_rune = ch;
}

static CVOID__PROTO(writerune, int ch, stream_node_t *s) {
  FILE *f = s->streamfile;
  if (s->isatty) {
    s = root_stream_ptr;
    /* ignore errors on tty */
    putc(ch, f);
  } else if (s->streammode != 's') { /* not a socket */
    if (putc(ch, f) < 0) {
      IO_ERROR("putc() in writerune()");
    }
  } else { /* a socket */
    char p;
    p = (char)ch;
    if (write(GetInteger(s->label), &p, (size_t)1) < 0) {
      IO_ERROR("write() in writerune()");
    }
  }
  inc_counts(ch,s);
}

static CVOID__PROTO(writerunen, int ch, int i, stream_node_t *s) {
  while (--i >= 0) {
    writerune(Arg, ch, s);
  }
}

static CVOID__PROTO(writebyte, int ch, stream_node_t *s) {
  FILE *f = s->streamfile;
  if (s->isatty) {
    s = root_stream_ptr;
    /* ignore errors on tty */
    putc(ch, f);
  } else if (s->streammode != 's') { /* not a socket */
    if (putc(ch, f) < 0) {
      IO_ERROR("putc() in writebyte()");
    }
  } else { /* a socket */
    char p;
    p = (char)ch;
    if (write(GetInteger(s->label), &p, (size_t)1) < 0) {
      IO_ERROR("write() in writebyte()");
    }
  }
}

#define DELRET -5
#define PEEK   -4
#define GET    -3
#define GET1   -2
#define SKIPLN -1

static inline int read_exit_cond(int op_type, c_rune_t r) {
  return (op_type<GET1 ||
          r==RUNE_EOF ||
          (op_type==GET1 && get_rune_class(r)>0) ||
          (op_type==SKIPLN && (r==0xa || r==0xd)) ||
          op_type==r);
}

static inline int read_give_back_cond(int op_type, c_rune_t r) {
  return (op_type==PEEK ||
          (op_type==SKIPLN && r==RUNE_EOF) ||
          (op_type==DELRET && r!=0xa));
}

/* Returns RUNE_PAST_EOF when attempting to read past end of file)

   op_type: DELRET, PEEK, GET, GET1, SKIPLN, or >= 0 for SKIP
 */
static CFUN__PROTO(readrune, int, stream_node_t *s, int op_type,
                   definition_t *pred_address) {
  FILE *f = s->streamfile;
  c_rune_t r;

  if (s->isatty) {
    int_address = pred_address;
    while (TRUE) {
      if (root_stream_ptr->rune_count==root_stream_ptr->last_nl_pos) {
        print_string(Arg, stream_user_output,GetString(current_prompt));
          /* fflush(stdout); into print_string() MCL */
      }

      if (s->pending_rune == RUNE_VOID) { /* There is no char returned by peek */
        /* ignore errors in tty */
        r = c_getc(f);
      } else {
        r = s->pending_rune;
        s->pending_rune = RUNE_VOID;
      }
      
      if (read_give_back_cond(op_type,r)) {
        s->pending_rune = r;
      } else {
        inc_counts(r,root_stream_ptr);
      }

      if (r==RUNE_EOF) clearerr(f);

      if (read_exit_cond(op_type,r)) {
        int_address = NULL; 
        return r;
      }
    }
  } else if (s->streammode != 's') { /* not a socket */
    if (s->pending_rune == RUNE_VOID && feof(f)) {
      return RUNE_PAST_EOF; /* attempt to read past end of stream */
    }
    
    while (TRUE) {
      if (s->pending_rune != RUNE_VOID) { /* There is a char returned by peek */
        r = s->pending_rune;
        s->pending_rune = RUNE_VOID;
      } else {
        r = c_getc(f);
        if (r < 0 && ferror(f)) {
          IO_ERROR("getc() in readrune()");
        }
      }

      if (read_give_back_cond(op_type,r)) {
        s->pending_rune = r;
      } else {
        inc_counts(r,s);
      }
      
      if (read_exit_cond(op_type,r)) return r;
    }
  } else { /* a socket */
    int fildes = GetInteger(s->label);
    
    if (s->socket_eof) return RUNE_PAST_EOF; /* attempt to read past end of stream */
    
    while (TRUE) {
      unsigned char ch;
      if (s->pending_rune == RUNE_VOID) { /* There is a char returned by peek */
        switch(read(fildes, (void *)&ch, 1)) {
        case 0:
          r = RUNE_EOF;
          break;
        case 1: 
          r = (int)ch;
          break;
        default:
          IO_ERROR("read() in readrune()");
        }
      } else {
        r = s->pending_rune;
        s->pending_rune = RUNE_VOID;
      }
      
      if (read_give_back_cond(op_type,r)) {
        s->pending_rune = r;
      } else {
        inc_counts(r,s);
        if (r==RUNE_EOF) s->socket_eof = TRUE;
      }

      if (read_exit_cond(op_type,r)) return r;
    }
  }
}

static CFUN__PROTO(readbyte, int, stream_node_t *s, int op_type,
                   definition_t *pred_address) {
  FILE *f = s->streamfile;
  int i;

  if (s->isatty) {
    int_address = pred_address;
    if (root_stream_ptr->rune_count==root_stream_ptr->last_nl_pos) {
      print_string(Arg, stream_user_output,GetString(current_prompt));
      /* fflush(stdout); into print_string() MCL */
    }

    if (s->pending_rune == RUNE_VOID) { /* There is no char returned by peek */
      /* ignore errors in tty */
      i = c_getc(f);
    } else {
      i = s->pending_rune;
      s->pending_rune = RUNE_VOID;
    }
      
    if (op_type == PEEK) { /* read_give_back_cond */
      s->pending_rune = i;
    }

    if (i==BYTE_EOF) clearerr(f);
    
    int_address = NULL; 
    return i;
  } else if (s->streammode != 's') { /* not a socket */
    if (s->pending_rune == RUNE_VOID && feof(f)) {
      return BYTE_PAST_EOF; /* attempt to read past end of stream */
    }
    if (s->pending_rune != RUNE_VOID) { /* There is a char returned by peek */
      i = s->pending_rune;
      s->pending_rune = RUNE_VOID;
    } else {
      i = c_getc(f);
      if (i < 0 && ferror(f)) {
        IO_ERROR("getc() in readbyte()");
      }
    }
    if (op_type == PEEK) { /* read_give_back_cond */
      s->pending_rune = i;
    }
    return i;
  } else { /* a socket */
    unsigned char ch;
    int fildes = GetInteger(s->label);
    
    if (s->socket_eof) return BYTE_PAST_EOF; /* attempt to read past end of stream */
    
    if (s->pending_rune == RUNE_VOID) { /* There is a char returned by peek */
      switch(read(fildes, (void *)&ch, 1)) {
      case 0:
        i = BYTE_EOF;
        break;
      case 1: 
        i = (int)ch;
        break;
      default:
        IO_ERROR("read() in readbyte()");
      }
    } else {
      i = s->pending_rune;
      s->pending_rune = RUNE_VOID;
    }

    if (op_type == PEEK) { /* read_give_back_cond */
      s->pending_rune = i;
    } else {
      if (i==BYTE_EOF) s->socket_eof = TRUE;
    }

    return i;
  }
}


/*----------------------------------------------------------------*/

CBOOL__PROTO(flush_output) {
  if (Output_Stream_Ptr->streammode != 's') { /* not a socket */
    if (fflush(Output_Stream_Ptr->streamfile)) {
      ENG_perror("fflush in flush_output/1");
    }
  }
  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(flush_output1) {
  ERR__FUNCTOR("stream_basic:flush_output", 1);
  int errcode;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'w', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  if (s->streammode != 's') { /* not a socket */
    if (fflush(s->streamfile)) {
      ENG_perror("fflush in flush_output/1");
    }
  }
  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(get) {
  ERR__FUNCTOR("io_basic:get_code", 1);
  c_rune_t r;

  r = readrune(Arg,Input_Stream_Ptr,GET,address_get);
  if (r == RUNE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
  }

  return cunify(Arg,X(0),MakeSmall(r));
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(get2) {
  ERR__FUNCTOR("io_basic:get_code", 2);
  c_rune_t r;
  int errcode;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'r', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  r = readrune(Arg,s,GET,address_get2);
  if (r == RUNE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),X(0),1);
  }

  return cunify(Arg,X(1),MakeSmall(r));
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(get1) {
  ERR__FUNCTOR("io_basic:get1_code", 1);
  c_rune_t r;
  
  r = readrune(Arg,Input_Stream_Ptr,GET1,address_get1); /* skip whitespace */
  if (r == RUNE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
  }

  return cunify(Arg,X(0),MakeSmall(r));
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(get12) {
  ERR__FUNCTOR("io_basic:get1_code", 2);
  c_rune_t r;
  int errcode;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'r', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  r = readrune(Arg,s,GET1,address_get12); /* skip whitespace */
  if (r == RUNE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),X(0),1);
  }

  return cunify(Arg,X(1),MakeSmall(r));
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(peek) {
  ERR__FUNCTOR("io_basic:peek_code", 1);
  c_rune_t r;

  r = readrune(Arg,Input_Stream_Ptr,PEEK,address_peek);
  if (r == RUNE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
  }

  return cunify(Arg,X(0),MakeSmall(r));
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(peek2) {
  ERR__FUNCTOR("io_basic:peek_code", 2);
  c_rune_t r;
  int errcode;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'r', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  r = readrune(Arg,s,PEEK,address_peek2);
  if (r == RUNE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),X(0),1);
  }

  return cunify(Arg,X(1),MakeSmall(r));
}

/*----------------------------------------------------------------*/

/* Read a UTF8 rune (return value) and assign a rune class to *typ */
static CFUN__PROTO(readrune_mb, c_rune_t,
                   stream_node_t *s, int op_type,
                   definition_t *pred_address, int *typ) {
  c_rune_t r;
  int typ_;
  
 again:
  r = CFUN__EVAL(readrune,s,op_type,pred_address);
  if (r<0) { *typ = -1; return r; }
  if (r <= 0x7F) { /* 1 byte */
  } else if (r <= 0xBF) {
    r = RUNE_ERROR;
  } else { /* 2 or more bytes */
    unsigned char b[4];
    int len;
    /* get length and read pending bytes */
    if (r <= 0xDF) { len=2; }
    else if (r <= 0xEF) { len=3; }
    else if (r <= 0xF7) { len=4; }
    else { len=0; }
    b[0] = (unsigned char)r;
    for (int i=1; i<len; i++) {
      r = CFUN__EVAL(readrune,s,GET,pred_address);
      if (r<0 || (r&0xC0)!=0x80) { len=0; break; } /* force error */
      b[i] = (unsigned char)r;
    }
    /* compose rune */
    switch(len) {
    case 2: /* 2 bytes */
      r = ((b[0]&0x1F)<<6)|(b[1]&0x3F);
      if (r < 0x80) r = RUNE_ERROR;
      break;
    case 3: /* 3 bytes */
      r = ((b[0]&0xF)<<12)|((b[1]&0x3F)<<6)|(b[2]&0x3F);
      if (r < 0x800) r = RUNE_ERROR;
      break;
    case 4: /* 4 bytes */
      r = ((b[0]&0x7)<<18)|((b[1]&0x3F)<<12)|((b[2]&0x3F)<<6)|(b[3]&0x3F);
      if (r < 0x10000) r = RUNE_ERROR;
      break;
    default:
      r = RUNE_ERROR; 
      break;
    }
  }
  typ_ = get_rune_class(r);
  if (op_type == GET1 && typ_ == RUNETY_LAYOUT) goto again;
  *typ = typ_;
  return r;
}

CBOOL__PROTO(getct) {
  ERR__FUNCTOR("io_basic:getct", 2);
  c_rune_t r;
  int typ;
  r = CFUN__EVAL(readrune_mb,Input_Stream_Ptr,GET,address_getct,&typ);
  if (r == RUNE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
  }
  return cunify(Arg,X(0),MakeSmall(r)) && cunify(Arg,X(1),MakeSmall(typ));
}

CBOOL__PROTO(getct1) {
  ERR__FUNCTOR("io_basic:getct1", 2);
  c_rune_t r;
  int typ;
  r = CFUN__EVAL(readrune_mb,Input_Stream_Ptr,GET1,address_getct1,&typ);
  if (r == RUNE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
  }
  return cunify(Arg,X(0),MakeSmall(r)) && cunify(Arg,X(1),MakeSmall(typ));
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(nl) {
  writerune(Arg, '\n',Output_Stream_Ptr);
  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(nl1) {
  ERR__FUNCTOR("io_basic:nl", 1);
  int errcode;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'w', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  writerune(Arg, '\n',s);
  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(put) {
  ERR__FUNCTOR("io_basic:put_code", 1);
  c_rune_t r;

  DEREF(X(0), X(0));
  CheckGetRune(X(0),r,1);
  writerune(Arg, r, Output_Stream_Ptr);

  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(put2) {
  ERR__FUNCTOR("io_basic:put_code", 2);
  c_rune_t r;
  int errcode;
  stream_node_t *s;

  s = stream_to_ptr_check(X(0), 'w', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  DEREF(X(1), X(1));
  CheckGetRune(X(1),r,2);
  writerune(Arg, r, s);

  return TRUE;
}

/*----------------------------------------------------------------*/
/* output stream always write or append */

CBOOL__PROTO(tab) {
  ERR__FUNCTOR("io_basic:tab", 1);
  DEREF(X(0),X(0));
  if (!IsInteger(X(0))) {
    ERROR_IN_ARG(X(0),1,INTEGER);
  }

  writerunen(Arg, ' ', GetInteger(X(0)), Output_Stream_Ptr);
  return TRUE;
}


/*----------------------------------------------------------------*/

CBOOL__PROTO(tab2) {
  ERR__FUNCTOR("io_basic:tab", 2);
  int errcode;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'w', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  DEREF(X(1),X(1));
  if (!IsInteger(X(1))) {
    ERROR_IN_ARG(X(1),2,INTEGER);
  }

  writerunen(Arg, ' ', GetInteger(X(1)), s);
  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(skip) {
  ERR__FUNCTOR("io_basic:skip_code", 1);
  c_rune_t r, r1;

  DEREF(X(0),X(0));
  CheckGetRune(X(0),r,1);

  for (r1=r+1; r1!=r;) {
    r1 = readrune(Arg,Input_Stream_Ptr,r,address_skip);
    if (r1 == RUNE_PAST_EOF) {
      BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
    }
  }

  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(skip2) {
  ERR__FUNCTOR("io_basic:skip_code", 2);
  c_rune_t r, r1;
  int errcode;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'r', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  DEREF(X(1),X(1))
  CheckGetRune(X(1),r,2);

  for (r1=r+1; r1!=r;) {
    r1 = readrune(Arg,s,r,address_skip2);
    if (r1 == RUNE_PAST_EOF) {
      BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),X(0),1);
    }
  }

  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(skip_line) {
  // ERR__FUNCTOR("io_basic:skip_line", 0);
  int r;

  for (r=0; r!=0xa && r!=0xd && r>=0;) {
    r = readrune(Arg,Input_Stream_Ptr,SKIPLN,address_skip_line);
  }

  if (r == 0xd) { /* Delete a possible 0xa (win end-of-line) */
    readrune(Arg,Input_Stream_Ptr,DELRET,address_skip_line);
  }

  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(skip_line1) {
  ERR__FUNCTOR("io_basic:skip_line", 1);
  int errcode;
  c_rune_t r;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'r', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  for (r=0; r!=0xa && r!=0xd && r>=0;) {
    r = readrune(Arg,s,SKIPLN,address_skip_line1);
  }

  if (r == 0xd) { /* Delete a possible 0xa (win end-of-line) */
    readrune(Arg,s,DELRET,address_skip_line1);
  }

  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(get_byte1) {
  ERR__FUNCTOR("io_basic:get_byte", 1);
  int i;

  i = readbyte(Arg,Input_Stream_Ptr,GET,address_get_byte1);
  if (i == BYTE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
  }

  return cunify(Arg,X(0),MakeSmall(i));
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(get_byte2) {
  ERR__FUNCTOR("io_basic:get_byte", 2);
  int i, errcode;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'r', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  i = readbyte(Arg,s,GET,address_get_byte2);
  if (i == BYTE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),X(0),1);
  }

  return cunify(Arg,X(1),MakeSmall(i));
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(peek_byte1) {
  ERR__FUNCTOR("io_basic:peek_byte", 1);
  int i;

  i = readbyte(Arg,Input_Stream_Ptr,PEEK,address_peek_byte1);
  if (i == BYTE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
  }

  return cunify(Arg,X(0),MakeSmall(i));
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(peek_byte2) {
  ERR__FUNCTOR("io_basic:peek_byte", 2);
  int i, errcode;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'r', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  i = readbyte(Arg,s,PEEK,address_peek_byte2);
  if (i == BYTE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),X(0),1);
  }

  return cunify(Arg,X(1),MakeSmall(i));
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(put_byte1) {
  ERR__FUNCTOR("io_basic:put_byte", 1);
  int i;

  DEREF(X(0),X(0));
  CheckGetByte(X(0),i,1);;
  writebyte(Arg,i,Output_Stream_Ptr);

  return TRUE;
}

/*----------------------------------------------------------------*/

CBOOL__PROTO(put_byte2) {
  ERR__FUNCTOR("io_basic:put_byte", 2);
  int i;
  int errcode;
  stream_node_t *s;

  s = stream_to_ptr_check(X(0), 'w', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  DEREF(X(1),X(1));
  CheckGetByte(X(1),i,2);;
  writerune(Arg, i, s);

  return TRUE;
}

/*----------------------------------------------------------------*/

// TODO: set dopeek=FALSE for end_of_stream(_) property

/* Return RUNE_EOF if we are 'at'-end-of-stream, RUNE_PAST_EOF if we
   are 'past'-end-of-stream or 0 otherwise. If 'dopeek' then a byte
   will be peeked (if possible) to determine if the stream has data. */
static CFUN__PROTO(stream_end_of_stream, int, stream_node_t *s, bool_t dopeek) {
  FILE *f = s->streamfile;
  int i;

  if (s->streammode != 's') { /* not a socket */
    if (s->pending_rune == RUNE_VOID && feof(f)) {
      return RUNE_PAST_EOF; /* attempt to read past end of stream */
    }
    if (s->pending_rune != RUNE_VOID) { /* There is a char returned by peek */
      return (s->pending_rune == RUNE_EOF ? RUNE_EOF : 0);
    } else {
      /* TODO: "pending_rune" should be a small byte buffer */
      if (dopeek) { /* peek a byte */
        i = c_getc(f);
        if (i < 0) { /* EOF */
          if (s->isatty) { /* ignore errors in tty */ /* TODO: sure? */
            clearerr(f);
          } else {
            if (ferror(f)) IO_ERROR("getc() in stream_end_of_stream()");
          }
          s->pending_rune = i;
          return RUNE_EOF;
        } else { /* not EOF */
          s->pending_rune = i; /* TODO: pending byte */
        }
      }
      return 0;
    }
  } else { /* a socket */
    unsigned char ch;
    int fildes = GetInteger(s->label);

    if (s->socket_eof) return RUNE_PAST_EOF; /* attempt to read past end of stream */

    if (s->pending_rune == RUNE_VOID) { /* There is a char returned by peek */
      if (dopeek) { /* peek a byte */
        switch(read(fildes, (void *)&ch, 1)) {
        case 0:
          i = BYTE_EOF;
          break;
        case 1: 
          i = (int)ch;
          break;
        default:
          IO_ERROR("read() in readbyte()");
        }
        if (i < 0) { /* EOF */
          s->pending_rune = i;
          return RUNE_EOF;
        } else { /* not EOF */
          s->pending_rune = i; /* TODO: pending byte */
        }
      }
      return 0;
    } else {
      return (s->pending_rune == RUNE_EOF ? RUNE_EOF : 0);
    }
  }
}

CBOOL__PROTO(at_end_of_stream0) {
  //ERR__FUNCTOR("io_basic:at_end_of_stream0", 0);
  return CFUN__EVAL(stream_end_of_stream, Input_Stream_Ptr, TRUE) < 0;
}

CBOOL__PROTO(at_end_of_stream1) {
  ERR__FUNCTOR("io_basic:at_end_of_stream1", 1);
  int errcode;
  stream_node_t *s;

  s = stream_to_ptr_check(X(0), 'r', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  return CFUN__EVAL(stream_end_of_stream, s, TRUE) < 0;
}

/*----------------------------------------------------------------*/
/* NOTE: Moved from stream_basic.c (DCG) */
/*----------------------------------------------------------------*/

/* TODO: should fflush() be moved where it is needed? slow? */

/* This is essentially an open-coded fputs().  
   fputs() starts paying off at string lengths above 50 or so.
 */
CVOID__PROTO(print_string, stream_node_t *stream, char *p) {
  FILE *fileptr = stream->streamfile;
  c_rune_t r;

  if (stream->isatty) {
    stream = root_stream_ptr;
    for (r = *p++; r; r = *p++) {
      /* ignore errors on tty */
      putc(r,fileptr);
      inc_counts(r,stream);
    }
  } else if (stream->streammode != 's') { /* not a socket */
    for (r = *p++; r; r = *p++) {
      if (putc(r,fileptr) < 0) {
        IO_ERROR("putc() in in print_string()");
      }
      inc_counts(r,stream);
    }
  } else { /* a socket */
    size_t size = 0;
    char *q = p;

    for (r = *q++; r; r = *q++) {
      inc_counts(r,stream);
      size++;
    }
    if (write(GetInteger(stream->label), p, size) < 0) {
      IO_ERROR("write() in print_string()");
    }
  }
  fflush(fileptr);
}

CVOID__PROTO(print_variable, stream_node_t *stream, tagged_t term) {
  number_to_string(Arg, var_address(Arg, term), 10);
  print_string(Arg, stream, "_");
  print_string(Arg, stream, Atom_Buffer);
}

CVOID__PROTO(print_number, stream_node_t *stream, tagged_t term) {
  number_to_string(Arg,term, 10);
  print_string(Arg, stream, Atom_Buffer);
}

#define FULL_ESCAPE_QUOTED_ATOMS 1

/* Max size of printed atom (+3 due to 0'', ..., 0'', 0'\0) */
#define PRINT_ATOM_BUFF_SIZE PRINT_RUNE_BUFF_SIZE*MAXATOM+3
/* Max size of a single printed rune */
#if defined(FULL_ESCAPE_QUOTED_ATOMS)
#define PRINT_RUNE_BUFF_SIZE 5 /* (e.g., 0'1  => 0'\\ 0'0 0'0 0'1 0'\\ */
#else
#define PRINT_RUNE_BUFF_SIZE 2 /* (e.g., 0'\n => 0'\\ 0'n */
#endif      

#define PRINT_CONTROL_RUNE(X) { *bp++ = '\\'; *bp++ = (X); }

CVOID__PROTO(print_atom, stream_node_t *stream, tagged_t term) {
  atom_t *atomptr = TagToAtom(term);

  if (!atomptr->has_special) {
    print_string(Arg, stream, atomptr->name);
  } else {
#if defined(USE_DYNAMIC_ATOM_SIZE)
    char *buf = checkalloc_ARRAY(char, PRINT_ATOM_BUFF_SIZE);
#else
    char buf[PRINT_ATOM_BUFF_SIZE]; 
#endif
    unsigned char *ch = (unsigned char *)atomptr->name;
    char *bp = buf;
    c_rune_t r;
      
    *bp++ = '\'';
#if defined(FULL_ESCAPE_QUOTED_ATOMS)
    while ((r = *ch++)) {
      /* See tokenize.pl for table of symbolic control chars */
      if (r <= 0x7F && get_rune_class(r) == 0) { /* TODO: only for ASCII, is it OK? */
        switch (r) {
        case 7: PRINT_CONTROL_RUNE('a'); break;
        case 8: PRINT_CONTROL_RUNE('b'); break;
        case 9: PRINT_CONTROL_RUNE('t'); break;
        case 10: PRINT_CONTROL_RUNE('n'); break;
        case 11: PRINT_CONTROL_RUNE('v'); break;
        case 12: PRINT_CONTROL_RUNE('f'); break;
        case 13: PRINT_CONTROL_RUNE('r'); break;
          /* case 27: PRINT_CONTROL_RUNE('e'); break; */
        case 32: *bp++ = ' '; break;
          /* case 127: PRINT_CONTROL_RUNE('d'); break; */
        default:
          *bp++ = '\\';
          *bp++ = '0' + ((r >> 6) & 7);
          *bp++ = '0' + ((r >> 3) & 7);
          *bp++ = '0' + (r & 7);
          *bp++ = '\\';
        }
      } else {
        if (r=='\'' || r=='\\') { *bp++ = r; }
        *bp++ = r;
      }
    }
#else
    if (atomptr->has_squote) {
      while ((r = *ch++)) {
        if (r=='\'' || r=='\\') { *bp++ = r; }
        *bp++ = r;
      }
    } else {
      while ((r = *ch++)) {
        if (r=='\\') { *bp++ = r; }
        *bp++ = r;
      }
    }
#endif
    *bp++ = '\'';
    *bp++ = 0;
    print_string(Arg, stream, buf);
#if defined(USE_DYNAMIC_ATOM_SIZE)
    checkdealloc_ARRAY(char, 2*MAXATOM+3, buf);
#endif
  }
}

/*   --------------------------------------------------------------  */  

CVOID__PROTO(display_term,
             tagged_t term,
             stream_node_t *stream,
             bool_t quoted) {
  tagged_t aux;
  int arity,i;

  switch (TagOf(term)) {
  case LST:
    writerune(Arg,'[',stream);
    DerefCar(aux,term);
    display_term(Arg,aux, stream, quoted);
    DerefCdr(term,term);
    while(TagIsLST(term)) {
      writerune(Arg,',',stream);
      DerefCar(aux,term);
      display_term(Arg,aux, stream, quoted);
      DerefCdr(term,term);
    }
    if (term!=atom_nil) {
      writerune(Arg,'|',stream);
      display_term(Arg,term, stream, quoted);
    }
    writerune(Arg,']',stream);
    break;
  case STR:
    if (STRIsLarge(term)) goto number;
    display_term(Arg,TagToHeadfunctor(term),stream, quoted);
    writerune(Arg,'(',stream);
    arity = Arity(TagToHeadfunctor(term));
    for (i=1; i<=arity; i++) {
      if (i>1) writerune(Arg,',',stream);
      DerefArg(aux,term,i);
      display_term(Arg,aux, stream, quoted);
    }
    writerune(Arg,')',stream);
    break;
  case UBV:
  case SVA:
  case HVA:
  case CVA:
    {
      print_variable(Arg,stream,term);
      break;
    }
  case ATM:
    if (quoted) {
      print_atom(Arg,stream,term);
    } else {
      print_string(Arg, stream,TagToAtom(term)->name);
    }
    break;
  case NUM:
  number:
    print_number(Arg,stream,term);
    break;
  }
}

CBOOL__PROTO(prolog_display)
{
  DEREF(X(0),X(0));
  display_term(Arg,X(0),Output_Stream_Ptr, FALSE);
  return TRUE;
}

CBOOL__PROTO(prolog_display2) {
  ERR__FUNCTOR("io_basic:display", 2);
  int errcode;
  stream_node_t *stream;
  
  stream = stream_to_ptr_check(X(0), 'w', &errcode);
  if (stream==NULL) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  DEREF(X(1),X(1));
  display_term(Arg,X(1),stream, FALSE);
  return TRUE;
}

CBOOL__PROTO(prolog_displayq) {
  DEREF(X(0),X(0));
  display_term(Arg,X(0), Output_Stream_Ptr, TRUE);
  return TRUE;
}

CBOOL__PROTO(prolog_displayq2) {
  ERR__FUNCTOR("io_basic:displayq", 2);
  int errcode;
  stream_node_t *stream;
  
  stream = stream_to_ptr_check(X(0), 'w', &errcode);
  if (stream==NULL) {
    BUILTIN_ERROR(errcode,X(0),1);
  }

  DEREF(X(1),X(1));
  display_term(Arg,X(1), stream, TRUE);
  return TRUE;
}

CBOOL__PROTO(prolog_clearerr) {
  ERR__FUNCTOR("stream_basic:clearerr", 1);
  int errcode;
  stream_node_t *s;
  
  s = stream_to_ptr_check(X(0), 'r', &errcode);
  if (!s) {
    BUILTIN_ERROR(errcode,X(0),1);
  }
  
  if (s->streammode != 's') { /* not a socket */
    clearerr(s->streamfile);
  }

  return TRUE;
}

/*----------------------------------------------------*/

#define FASTRW_VERSION  'C'
#define FASTRW_MAX_VARS 1024

#define SPACE_FACTOR 64  /* kludge to ensure more heap space before reading */

CBOOL__PROTO(prolog_fast_read_in_c_aux, 
             tagged_t *out,
             tagged_t *vars,
             int *lastvar);

/* OPA */
CBOOL__PROTO(prolog_fast_read_in_c) {
  ERR__FUNCTOR("fastrw:fast_read", 1);
  int i,lastvar = 0;
  tagged_t term, vars[FASTRW_MAX_VARS];

  /* MCL, JC: Changed getc() to readbyte() because of wrong assumptions when
     using sockets (i.e., streamfile = NULL.  */

  /* NULL as predaddress (really did not bother to find out what to put)  */

  i = readbyte(Arg, Input_Stream_Ptr, GET, NULL);
  if (i == BYTE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
  }
  if (i != FASTRW_VERSION) return FALSE;

  ENSURE_HEAP(SPACE_FACTOR*kCells, 1);

  if (!prolog_fast_read_in_c_aux(Arg,&term,vars,&lastvar)) return FALSE;

  return cunify(Arg,X(0),term);
}

#if defined(DEBUG)
#define CHECK_HEAP_SPACE                                        \
  if (HeapDifference(w->global_top,Heap_End) < CONTPAD) {       \
    fprintf(stderr, "Out of heap space in fast_read()\n");      \
  }
#else
#define CHECK_HEAP_SPACE
#endif

CBOOL__PROTO(prolog_fast_read_in_c_aux, 
             tagged_t *out,
             tagged_t *vars,
             int *lastvar) {
  ERR__FUNCTOR("fastrw:fast_read", 1);
  int i,k,j;
  unsigned char *s = (unsigned char *) Atom_Buffer;
  int base;
  
  k = readbyte(Arg, Input_Stream_Ptr, GET, NULL);
  if (k == BYTE_PAST_EOF) {
    BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
  }

  switch(k) {
  case ']':
    *out = atom_nil;
    CHECK_HEAP_SPACE;
    return TRUE;
  case '[':
    {
      tagged_t *h = w->global_top;
      w->global_top += 2;
      if (!prolog_fast_read_in_c_aux(Arg,h,vars,lastvar)) return FALSE;
      if (!prolog_fast_read_in_c_aux(Arg,h+1,vars,lastvar)) return FALSE;
      *out = Tag(LST,h);
    }
    CHECK_HEAP_SPACE;
    return TRUE;
  case '_':
  case 'I':
  case 'F':
  case 'A':
  case '"':
  case 'S':
    j = 1;
    for (i=0; j; i++) {
      if (i == Atom_Buffer_Length) {
        EXPAND_ATOM_BUFFER(Atom_Buffer_Length*2);
        s = (unsigned char *)Atom_Buffer+i;
      }
      j = readbyte(Arg, Input_Stream_Ptr, GET, NULL);
      if (j == BYTE_PAST_EOF) {
        BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
      }
      *s++ = j;
    }
    switch (k) {
    case '_':
      {
        tagged_t *h = w->global_top;
        if ((i = atoi(Atom_Buffer)) == *lastvar)
          *h = vars[(*lastvar)++] = TagHVA(w->global_top++);
        *out = vars[i];
      }
      CHECK_HEAP_SPACE;
      return TRUE;
    case 'I':
      base = GetSmall(current_radix);
      StringToInt(Atom_Buffer, base, *out, 1);
      CHECK_HEAP_SPACE;
      return TRUE;
    case 'F':
      string_to_number(Arg, Atom_Buffer, 10, out, 2);
      CHECK_HEAP_SPACE;
      return TRUE;
    case 'A':
      *out = GET_ATOM(Atom_Buffer);
      CHECK_HEAP_SPACE;
      return TRUE;
    case '"':
      {
        tagged_t *h = w->global_top;
        i--;
        /* ENSURE_HEAP_LST(i, 1); */
        while (i--) {
          MakeLST(*out,MakeSmall(((unsigned char *)Atom_Buffer)[i]),*out);
        }
        if (!prolog_fast_read_in_c_aux(Arg,h+1,vars,lastvar)) return FALSE;
      }
      CHECK_HEAP_SPACE;
      return TRUE;
    case 'S':
      i = readbyte(Arg, Input_Stream_Ptr, GET, NULL);
      if (i == BYTE_PAST_EOF) {
        BUILTIN_ERROR(PERMISSION_ERROR(ACCESS, PAST_END_OF_STREAM),atom_nil,0);
      }
      {
        tagged_t *h = w->global_top;
        /* ENSURE_HEAP(i+1, 1); */
        *h = SetArity(GET_ATOM(Atom_Buffer),i);
        *out = Tag(STR,h++);
        w->global_top += i+1;
        while(i--) {
          if (!prolog_fast_read_in_c_aux(Arg,h++,vars,lastvar)) return FALSE;
        }
      }
      CHECK_HEAP_SPACE;
      return TRUE;
    }
  default:
    return FALSE;
  }
}

static inline CVOID__PROTO(fast_write_string, stream_node_t *stream, const char * s) {
  for (;*s; s++) {
    writebyte(Arg, *s, stream);
  }
}

CVOID__PROTO(fast_write_number,
             stream_node_t *stream,
             tagged_t term) {
  number_to_string(Arg,term, 10);
  fast_write_string(Arg, stream, Atom_Buffer);
}

CVOID__PROTO(prolog_fast_write_in_c_aux,
             tagged_t in,
             tagged_t *vars, 
             int *lastvar);

/* OPA */
CBOOL__PROTO(prolog_fast_write_in_c) {
  tagged_t vars[FASTRW_MAX_VARS];
  int lastvar = 0;

  DEREF(X(0),X(0));
  writebyte(Arg, FASTRW_VERSION, Output_Stream_Ptr);
  prolog_fast_write_in_c_aux(Arg,X(0),vars,&lastvar);
  return TRUE;
}

CVOID__PROTO(prolog_fast_write_in_c_aux,
             tagged_t in,
             tagged_t *vars,
             int *lastvar) {
  int i, j;
  intmach_t b;
  tagged_t term;

  switch (TagOf(in)) {
  case LST:
    DerefCar(term,in);
    DerefCdr(in,in);
    if (TaggedIsSmall(term) && (b = GetSmall(term)))
      if ((b > 0) && (b < 256)) {
        for (writebyte(Arg,'"',Output_Stream_Ptr);(b > 0) && (b < 256);) {
          writebyte(Arg,b,Output_Stream_Ptr);
          if (TagOf(in) == LST) {
            DerefCar(term,in);
            DerefCdr(in,in);
            if (!TaggedIsSmall(term)) {
              break;
            } else {
              b = GetSmall(term);
            }
          } else {
            writebyte(Arg,0,Output_Stream_Ptr);
            prolog_fast_write_in_c_aux(Arg,in,vars,lastvar);
            return;
          }       
        }
        writebyte(Arg,0,Output_Stream_Ptr);
      }
    writebyte(Arg,'[',Output_Stream_Ptr);
    prolog_fast_write_in_c_aux(Arg,term,vars,lastvar);
    prolog_fast_write_in_c_aux(Arg,in,vars,lastvar);
    return;
  case UBV:
  case SVA:
  case HVA:
  case CVA:
    writebyte(Arg,'_',Output_Stream_Ptr);
    DEREF(in,in);
    for (i = 0;i < *lastvar; i++) {
      if (vars[i] == in) break;
    }
    if (i == *lastvar) {
      vars[(*lastvar)++] = in;
    }
    sprintf((char *) Atom_Buffer,"%i",i);
    fast_write_string(Arg,Output_Stream_Ptr,Atom_Buffer);
    writebyte(Arg,0,Output_Stream_Ptr);
    return;
  case STR:
    if (!STRIsLarge(in)) {
      writebyte(Arg,'S',Output_Stream_Ptr);
      fast_write_string(Arg,Output_Stream_Ptr,TagToAtom(TagToHeadfunctor(in))->name);
      writebyte(Arg,0,Output_Stream_Ptr);
      writebyte(Arg,j = Arity(TagToHeadfunctor(in)),Output_Stream_Ptr);
      for (i = 1; i <= j; prolog_fast_write_in_c_aux(Arg,term,vars,lastvar)) {
        DerefArg(term,in,i++);
      }
      return;
    }
  case NUM:
    if (IsFloat(in)) {
      writebyte(Arg,'F',Output_Stream_Ptr);
    } else {
      writebyte(Arg,'I',Output_Stream_Ptr);
    }
    fast_write_number(Arg,Output_Stream_Ptr,in);
    writebyte(Arg,0,Output_Stream_Ptr);
    return;
  case ATM:
    if (in != atom_nil) {
      writebyte(Arg,'A',Output_Stream_Ptr);
      fast_write_string(Arg,Output_Stream_Ptr,TagToAtom(in)->name);
      writebyte(Arg,0,Output_Stream_Ptr);
    } else {
      writebyte(Arg,']',Output_Stream_Ptr);
    }
    return;
  }
}

/*----------------------------------------------------*/

/* Routines for the compression and uncompression of bytecode, used on 
   the CIAO compiler (OPA) */ 

unsigned char sizeLZ(int n) {
  if (n > 2047) return 12;
  else if (n > 1023) return 11;
  else if (n > 511) return 10;
  else return 9;
}

CVOID__PROTO(outLZ,
             int *Buffer,
             char *BufferSize,
             int Code,
             unsigned char size) {
  Buffer[0] += Code*(1<<(BufferSize[0]));
  for (BufferSize[0] += size; BufferSize[0] >= 8; BufferSize[0] -= 8) {
    writebyte(Arg,Buffer[0] % 256,Output_Stream_Ptr);
    Buffer[0] /= 256;
  }
}

CBOOL__PROTO(compressLZ) {
  ERR__FUNCTOR("compressed_bytecode:compressLZ", 1);
  char *Dict[4096];
  char *First;
  char Vault[200000];
  char CarrySize = 0;
  int  i;
  int  Carry = 0;  
  int  Last = 256;
  int  PrefixSize = 0;
  int  Entry = 0;
  int  Size[4096];
  stream_node_t *s;
  FILE *f;
  
  s = stream_to_ptr_check(X(0), 'r', &i);
  if (!s) {
    BUILTIN_ERROR(i,X(0),1);
  }

  f = s->streamfile;
  
  writebyte(Arg,12,Output_Stream_Ptr);

  for (i = 0; i < 257; Size[i++] = 1) 
      { Dict[i] = &Vault[i];
        Vault[i] = i % 256; }
  First = &Vault[256];
  
  while((i = getc(f)) >= 0) {
    First[PrefixSize++] = i;
    for (i = Entry; Entry <= Last; Entry++) {
      if ((Size[Entry] == PrefixSize) && (Dict[Entry][0] == First[0])
          && !(memcmp(&Dict[Entry][1],&First[1],PrefixSize-1))) {
        break;
      }
    }
    if (Entry > Last) {
      Entry = First[PrefixSize-1];
      outLZ(Arg,&Carry,&CarrySize,i,sizeLZ(Last));
      if (Last == 4095) {
        First = &Vault[Last = 256];
      } else {
        Dict[++Last] = First;
        Size[Last] = PrefixSize;
        First += PrefixSize;
      }
      First[0] = Entry;
      PrefixSize = 1;
    }
  }

  if (ferror(f)) {
    IO_ERROR("getc() in compressLZ");
  }

  if (PrefixSize) {
    outLZ(Arg,&Carry,&CarrySize,Entry,sizeLZ(Last));
  }
  outLZ(Arg,&Carry,&CarrySize,256,sizeLZ(Last));
  outLZ(Arg,&Carry,&CarrySize,0,7);
  return TRUE;
}

static CVOID__PROTO(inLZ, FILE *f,
                    int *Buffer, char *BufferSize,
                    int *Code, char size) {
  //  ERR__FUNCTOR("compressed_bytecode:copyLZ", 1);
  int i;

  for (; BufferSize[0] < size; BufferSize[0] += 8) {
    i = getc(f);
    if (i < 0) {
      if (ferror(f)) {
        IO_ERROR("getc() in inLZ()");
      }
    } 
    Buffer[0] += ((unsigned char) i)*(1<<BufferSize[0]);
  }
  Code[0] = Buffer[0] % (1<<size);
  Buffer[0] /= (1<<size);
  BufferSize[0] -= size;
}

CBOOL__PROTO(copyLZ) {
  ERR__FUNCTOR("compressed_bytecode:copyLZ", 1);
  int  i;
  int  Last = 256;
  int  PrefixSize = 1;
  int  Carry = 0;
  char CarrySize = 0;
  char *Dict[4096];
  int  Size[4096];
  char *First;
  char Vault[200000];
  stream_node_t *s;
  FILE *f;
  
  s = stream_to_ptr_check(X(0), 'r', &i);
  if (!s) {
    BUILTIN_ERROR(i,X(0),1);
  }

  f = s->streamfile;
  
  i = getc(f);
  
  if (i != 12) {
    while (i >= 0) {
      writebyte(Arg,i,Output_Stream_Ptr);
      i = getc(f);
    }
    if (ferror(f)) {
      IO_ERROR("getc() in copyLZ()");
    }
    return TRUE;
  } else {
    for (i = 0; i < 257; Size[i++] = 1) {
      Dict[i] = &Vault[i];
      Vault[i] = i % 256;
    }
    First = &Vault[256];
 
    inLZ(Arg,f,&Carry,&CarrySize,&i,9);
    First[0] = i;
    while(1) {
      for (i = 0; i < PrefixSize;) {
        writebyte(Arg,First[i++],Output_Stream_Ptr);
      }
      inLZ(Arg,f,&Carry,&CarrySize,&i,sizeLZ(++Last % 4096));
      return FALSE;
      if (i == 256) return TRUE;
      if (Last == 4096) {
        (First = &Vault[Last = 256])[0] = i;
        PrefixSize = 1;
      } else {
        Size[Last] = PrefixSize+1;
        (Dict[Last] = First)[PrefixSize] = Dict[i][0];
        (void)memmove(First += Size[Last],Dict[i],PrefixSize = Size[i]);
      }
    }
  }
  return FALSE;
}

