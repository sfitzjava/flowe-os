/* FBPK9 codec 2. Shared bounded cursors; no allocation or page reconstruction.
 * Columns: u16 delta/tail/text byte lengths, u16 lines/images, line headers,
 * font/atlas-low/atlas-high columns, residual varints, tail, word text.
 * Tail keeps images, line-CID varints, word metadata and extension bytes.
 * PRED follows the dictionary: u32 count then sorted {u32 key,i32 delta}.
 * The previous glyph predicts the next advance; reset at each line. */
#ifndef FBP_COMPACT_H
#define FBP_COMPACT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#define FBP_COMPACT_VERSION 9
#define FBP_COMPACT_READER 6
#define FBP_COMPACT_CODEC 2
#define FBP_PREDICTOR_CAP 1024
#define FBP_BODY_CAP 32768
#define FBP_PROFILE_CAP 32
static inline uint16_t fc_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static inline uint32_t fc_u32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static inline int fc_var(const uint8_t **p,const uint8_t *end,int32_t *v) {
  uint32_t u=0;
  for(unsigned s=0;s<=28;s+=7) {
    if(*p==end) return 0;
    uint8_t b=*(*p)++;
    if(s==28 && (b&0xf0)) return 0;
    u|=(uint32_t)(b&127)<<s;
    if(!(b&128)) { *v=(int32_t)(u>>1)^-(int32_t)(u&1); return 1; }
  }
  return 0;
}
static inline int fc_add(int32_t a,int32_t b,int32_t *out) {
  int64_t n=(int64_t)a+b;
  if(n<INT32_MIN || n>INT32_MAX) return 0;
  *out=(int32_t)n; return 1;
}
#pragma pack(push, 1)
typedef struct { uint32_t key; int32_t delta; } FcPredictor;
#pragma pack(pop)
static inline int fc_table_valid(const FcPredictor *t,uint32_t n) {
  if(n>FBP_PREDICTOR_CAP) return 0;
  for(uint32_t i=0;i<n;i++)
    if(t[i].key>=0x80000u || (i && t[i-1].key>=t[i].key) ||
       t[i].delta < -65535 || t[i].delta > 65535) return 0;
  return 1;
}
static inline int32_t fc_predict(const FcPredictor *t,uint32_t n,uint32_t key) {
  uint32_t lo=0,hi=n;
  while(lo<hi) { uint32_t m=lo+(hi-lo)/2; if(t[m].key<key) lo=m+1; else hi=m; }
  return lo<n && t[lo].key==key ? t[lo].delta : 0;
}
typedef struct {
  const uint8_t *base,*end,*lines,*fonts,*low,*high,*positions,*positions_end,*tail,*tail_end,*text,*text_end;
  uint32_t glyphs;
  uint16_t nlines,nimages;
} FcPage;
typedef struct { const FcPage *page; const uint8_t *pos; uint32_t glyph,previous; int32_t x; } FcGlyphCursor;
static inline void fc_cursor(FcGlyphCursor *c,const FcPage *p) { c->page=p;c->pos=p->positions;c->glyph=0;c->previous=UINT32_MAX;c->x=0; }
static inline void fc_line(FcGlyphCursor *c) { c->x=0;c->previous=UINT32_MAX; }
static inline int fc_glyph(FcGlyphCursor *c,const FcPredictor *t,uint32_t n,uint8_t *font,uint16_t *index,int32_t *x) {
  if(c->glyph>=c->page->glyphs) return 0;
  uint32_t g=c->glyph++;
  *font=c->page->fonts[g];*index=(uint16_t)(c->page->low[g]|((uint16_t)c->page->high[g]<<8));
  int32_t dx;
  if(!fc_var(&c->pos,c->page->positions_end,&dx) ||
     !fc_add(dx,fc_predict(t,n,c->previous),&dx) || !fc_add(c->x,dx,&c->x) ||
     c->x<INT16_MIN || c->x>INT16_MAX || *font>=8) return 0;
  c->previous=((uint32_t)*font<<16)|*index;*x=c->x;return 1;
}
/* Validate the entire compact page before drawing any pixels. The tail
 * validator also bounds counts and word positions before on-demand reads. */
static inline int fc_page(FcPage *v,const uint8_t *p,size_t size,const FcPredictor *table,uint32_t entries) {
  memset(v,0,sizeof(*v));
  if(size<10 || size>FBP_BODY_CAP || !fc_table_valid(table,entries)) return 0;
  v->base=p;v->end=p+size;
  size_t dn=fc_u16(p),qn=fc_u16(p+2),wn=fc_u16(p+4);
  v->nlines=fc_u16(p+6);v->nimages=fc_u16(p+8);v->lines=p+10;
  if(v->nlines>64 || (size_t)v->nlines*4>size-10) return 0;
  size_t off=10+(size_t)v->nlines*4;
  for(uint16_t l=0;l<v->nlines;l++) v->glyphs+=fc_u16(v->lines+4*l+2);
  if(v->glyphs>(size-off)/3) return 0;
  v->fonts=p+off;v->low=v->fonts+v->glyphs;v->high=v->low+v->glyphs;
  off+=3*v->glyphs;
  if(dn>size-off || qn>size-off-dn || wn!=size-off-dn-qn) return 0;
  v->positions=p+off;v->positions_end=v->positions+dn;
  v->tail=v->positions_end;v->tail_end=v->tail+qn;v->text=v->tail_end;v->text_end=v->end;
  FcGlyphCursor c;fc_cursor(&c,v);
  for(uint16_t l=0;l<v->nlines;l++) {
    fc_line(&c);
    for(uint32_t g=0;g<fc_u16(v->lines+4*l+2);g++) {
      uint8_t f;uint16_t i;int32_t x;if(!fc_glyph(&c,table,entries,&f,&i,&x)) return 0;
    }
  }
  if(c.pos!=v->positions_end || (size_t)v->nimages*12>qn) return 0;
  const uint8_t *q=v->tail+(size_t)v->nimages*12,*text=v->text;int32_t n;
  for(uint16_t l=0;l<v->nlines;l++) if(!fc_var(&q,v->tail_end,&n)) return 0;
  for(uint16_t l=0;l<v->nlines;l++) {
    if(!fc_var(&q,v->tail_end,&n) || n<0 || (uint32_t)n>(size_t)(v->tail_end-q)/4) return 0;
    int32_t previous=0;
    for(int32_t w=0;w<n;w++) {
      int32_t dx,width,x;
      if(!fc_var(&q,v->tail_end,&dx) || !fc_var(&q,v->tail_end,&width) ||
         !fc_add(previous,dx,&x) || !fc_add(x,width,&previous) ||
         x<INT16_MIN || x>INT16_MAX || width<0 || width>INT16_MAX ||
         v->tail_end-q<2) return 0;
      uint8_t len=q[1];q+=2;
      if((size_t)(v->text_end-text)<len) return 0;
      text+=len;
    }
  }
  return text==v->text_end;
}
/* Incremental canonical-body CRC used by the native bench. Small varints
 * use five stack bytes; no interleaved page is constructed. */
static inline uint32_t fc_crc_bytes(uint32_t crc,const uint8_t *p,size_t n) {
  static const uint32_t t[16]={0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
    0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
  while(n--){crc^=*p++;crc=(crc>>4)^t[crc&15];crc=(crc>>4)^t[crc&15];}return crc;
}
static inline uint32_t fc_crc_var(uint32_t crc,int32_t v) {
  uint32_t u=((uint32_t)v<<1)^(uint32_t)(v>>31);uint8_t bytes[5];size_t n=0;
  while(u>=128){bytes[n++]=(uint8_t)(u|128);u>>=7;}bytes[n++]=(uint8_t)u;
  return fc_crc_bytes(crc,bytes,n);
}
static inline int fc_body_crc(const FcPage *v,const FcPredictor *table,uint32_t entries,uint32_t *crc) {
  uint32_t h=fc_crc_bytes(*crc,v->base+6,4);FcGlyphCursor c;fc_cursor(&c,v);
  for(uint16_t l=0;l<v->nlines;l++) {
    h=fc_crc_bytes(h,v->lines+4*l,4);fc_line(&c);int32_t prev=0;
    for(uint32_t g=0;g<fc_u16(v->lines+4*l+2);g++) {
      uint8_t f;uint16_t idx;int32_t x;if(!fc_glyph(&c,table,entries,&f,&idx,&x))return 0;
      uint8_t token[3]={f,(uint8_t)idx,(uint8_t)(idx>>8)};h=fc_crc_bytes(h,token,3);h=fc_crc_var(h,x-prev);prev=x;
    }
  }
  const uint8_t *q=v->tail,*text=v->text,*start=q;q+=(size_t)v->nimages*12;
  for(uint16_t l=0;l<v->nlines;l++){int32_t z;if(!fc_var(&q,v->tail_end,&z))return 0;}
  h=fc_crc_bytes(h,start,(size_t)(q-start));
  for(uint16_t l=0;l<v->nlines;l++) {
    start=q;int32_t count;if(!fc_var(&q,v->tail_end,&count))return 0;h=fc_crc_bytes(h,start,q-start);
    for(int32_t w=0;w<count;w++) {
      start=q;int32_t z;if(!fc_var(&q,v->tail_end,&z)||!fc_var(&q,v->tail_end,&z)||v->tail_end-q<2)return 0;
      uint8_t n=q[1];q+=2;h=fc_crc_bytes(h,start,q-start);h=fc_crc_bytes(h,text,n);text+=n;
    }
  }
  *crc=fc_crc_bytes(h,q,(size_t)(v->tail_end-q));return 1;
}

#endif
