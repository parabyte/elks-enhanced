/*
 * Upstream uIP DNS resolver.
 *
 * Vendored from adamdunkels/uip with ELKS integration changes limited to
 * coexistence with the existing unified UDP app dispatcher.
 */

#include "resolv.h"
#include "uip.h"

#include <string.h>

#ifndef NULL
#define NULL (void *)0
#endif

#define MAX_RETRIES 8

struct dns_hdr {
  u16_t id;
  u8_t flags1, flags2;
#define DNS_FLAG1_RESPONSE        0x80
#define DNS_FLAG1_RD              0x01
#define DNS_FLAG2_ERR_MASK        0x0f
  u16_t numquestions;
  u16_t numanswers;
  u16_t numauthrr;
  u16_t numextrarr;
};

struct dns_answer {
  u16_t type;
  u16_t class;
  u16_t ttl[2];
  u16_t len;
  uip_ipaddr_t ipaddr;
};

struct namemap {
#define STATE_UNUSED 0
#define STATE_NEW    1
#define STATE_ASKING 2
#define STATE_DONE   3
#define STATE_ERROR  4
  u8_t state;
  u8_t tmr;
  u8_t retries;
  u8_t seqno;
  u8_t err;
  char name[32];
  uip_ipaddr_t ipaddr;
};

#ifndef UIP_CONF_RESOLV_ENTRIES
#define RESOLV_ENTRIES 4
#else
#define RESOLV_ENTRIES UIP_CONF_RESOLV_ENTRIES
#endif

extern char resolv_appstate_tag;

static struct namemap names[RESOLV_ENTRIES];
static u8_t seqno;
static struct uip_udp_conn *resolv_conn = NULL;

static unsigned char *parse_name(unsigned char *query)
{
  unsigned char n;

  do {
    n = *query++;
    while(n > 0) {
      ++query;
      --n;
    }
  } while(*query != 0);
  return query + 1;
}

static void check_entries(void)
{
  struct dns_hdr *hdr;
  char *query, *nptr, *nameptr;
  static u8_t i;
  static u8_t n;
  struct namemap *namemapptr;

  if(resolv_conn == NULL) {
    return;
  }

  for(i = 0; i < RESOLV_ENTRIES; ++i) {
    namemapptr = &names[i];
    if(namemapptr->state == STATE_NEW ||
       namemapptr->state == STATE_ASKING) {
      if(namemapptr->state == STATE_ASKING) {
        if(--namemapptr->tmr == 0) {
          if(++namemapptr->retries == MAX_RETRIES) {
            namemapptr->state = STATE_ERROR;
            resolv_found(namemapptr->name, NULL);
            continue;
          }
          namemapptr->tmr = namemapptr->retries;
        } else {
          continue;
        }
      } else {
        namemapptr->state = STATE_ASKING;
        namemapptr->tmr = 1;
        namemapptr->retries = 0;
      }

      hdr = (struct dns_hdr *)uip_appdata;
      memset(hdr, 0, sizeof(*hdr));
      hdr->id = htons(i);
      hdr->flags1 = DNS_FLAG1_RD;
      hdr->numquestions = HTONS(1);
      query = (char *)uip_appdata + 12;
      nameptr = namemapptr->name;
      --nameptr;
      do {
        ++nameptr;
        nptr = query;
        ++query;
        for(n = 0; *nameptr != '.' && *nameptr != 0; ++nameptr) {
          *query = *nameptr;
          ++query;
          ++n;
        }
        *nptr = n;
      } while(*nameptr != 0);
      {
        static unsigned char endquery[] = {0,0,1,0,1};
        memcpy(query, endquery, sizeof(endquery));
      }
      uip_udp_send((unsigned char)(query + 5 - (char *)uip_appdata));
      break;
    }
  }
}

static void newdata(void)
{
  char *nameptr;
  struct dns_answer *ans;
  struct dns_hdr *hdr;
  static u8_t nanswers;
  static u8_t i;
  struct namemap *namemapptr;

  hdr = (struct dns_hdr *)uip_appdata;
  i = htons(hdr->id);
  if(i >= RESOLV_ENTRIES) {
    return;
  }
  namemapptr = &names[i];
  if(namemapptr->state != STATE_ASKING) {
    return;
  }

  namemapptr->state = STATE_DONE;
  namemapptr->err = hdr->flags2 & DNS_FLAG2_ERR_MASK;
  if(namemapptr->err != 0) {
    namemapptr->state = STATE_ERROR;
    resolv_found(namemapptr->name, NULL);
    return;
  }

  nanswers = htons(hdr->numanswers);
  nameptr = parse_name((unsigned char *)uip_appdata + 12) + 4;

  while(nanswers > 0) {
    if(*nameptr & 0xc0) {
      nameptr += 2;
    } else {
      nameptr = (char *)parse_name((unsigned char *)nameptr);
    }

    ans = (struct dns_answer *)nameptr;
    if(ans->type == HTONS(1) &&
       ans->class == HTONS(1) &&
       ans->len == HTONS(4)) {
      namemapptr->ipaddr[0] = ans->ipaddr[0];
      namemapptr->ipaddr[1] = ans->ipaddr[1];
      resolv_found(namemapptr->name, namemapptr->ipaddr);
      return;
    }
    nameptr = nameptr + 10 + htons(ans->len);
    --nanswers;
  }
}

void resolv_appcall(void)
{
  if(resolv_conn == NULL || uip_udp_conn != resolv_conn) {
    return;
  }
  if(uip_poll()) {
    check_entries();
  }
  if(uip_newdata()) {
    newdata();
  }
}

void resolv_query(char *name)
{
  static u8_t i;
  static u8_t lseq, lseqi;
  struct namemap *nameptr;

  lseq = lseqi = 0;
  for(i = 0; i < RESOLV_ENTRIES; ++i) {
    nameptr = &names[i];
    if(nameptr->state == STATE_UNUSED) {
      break;
    }
    if(seqno - nameptr->seqno > lseq) {
      lseq = seqno - nameptr->seqno;
      lseqi = i;
    }
  }

  if(i == RESOLV_ENTRIES) {
    i = lseqi;
    nameptr = &names[i];
  }

  strcpy(nameptr->name, name);
  nameptr->state = STATE_NEW;
  nameptr->seqno = seqno;
  ++seqno;
}

u16_t *resolv_lookup(char *name)
{
  static u8_t i;
  struct namemap *nameptr;

  for(i = 0; i < RESOLV_ENTRIES; ++i) {
    nameptr = &names[i];
    if(nameptr->state == STATE_DONE &&
       strcmp(name, nameptr->name) == 0) {
      return nameptr->ipaddr;
    }
  }
  return NULL;
}

u16_t *resolv_getserver(void)
{
  if(resolv_conn == NULL) {
    return NULL;
  }
  return resolv_conn->ripaddr;
}

void resolv_conf(u16_t (*dnsserver)[2])
{
	if(resolv_conn != NULL) {
		uip_udp_remove(resolv_conn);
	}

  resolv_conn = uip_udp_new(dnsserver, HTONS(53));
  if(resolv_conn != NULL) {
    resolv_conn->appstate = &resolv_appstate_tag;
  }
}

void resolv_init(void)
{
  static u8_t i;

  seqno = 0;
  for(i = 0; i < RESOLV_ENTRIES; ++i) {
    memset(&names[i], 0, sizeof(names[i]));
    names[i].state = STATE_UNUSED;
  }
}
