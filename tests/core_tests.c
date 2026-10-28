#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ophion_protocol.h"
#include "ophion_paging.h"
#include "ophion_memtype.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static uint64_t tables[4][512];
static int reader(void *unused, uint64_t pa, uint64_t *value) {
    unsigned page = (unsigned)(pa >> 12);
    (void)unused;
    if (page < 1 || page > 4 || (pa & 7)) return 0;
    *value = tables[page-1][(pa & 4095)/8]; return 1;
}
static void setup(void) {
    memset(tables, 0, sizeof(tables));
    tables[0][0] = 0x2007; tables[1][0] = 0x3007;
    tables[2][0] = 0x4007; tables[3][1] = 0xabc007;
}
int main(void) {
    OPH_PACKET p = {0}; OPH_TRANSLATION t;
    CHECK(sizeof(p) == 1136); CHECK(offsetof(OPH_PACKET, data) == 112);
    CHECK(sizeof(OPH_EVENT) * OPH_EVENT_BATCH <= OPH_DATA_SIZE);
    CHECK(OPH_EXIT_BUCKETS * sizeof(uint64_t) <= OPH_DATA_SIZE);
    CHECK(!oph_packet_valid(NULL)); CHECK(!oph_packet_valid(&p));
    p.magic = OPH_MAGIC; p.version = OPH_VERSION; p.size = sizeof(p); p.operation = OPH_PING;
    CHECK(oph_packet_valid(&p)); p.version++; CHECK(!oph_packet_valid(&p)); p.version--;
    p.size--; CHECK(!oph_packet_valid(&p)); p.size++; p.operation = 999; CHECK(!oph_packet_valid(&p));
    CHECK(oph_range_valid(4095,1)); CHECK(oph_range_valid(3072,1024));
    CHECK(!oph_range_valid(4096,1)); CHECK(!oph_range_valid(UINT64_MAX,2));
    CHECK(!oph_range_valid(0,UINT64_MAX)); CHECK(!oph_range_valid(0,0));
    CHECK(!oph_range_valid(0,1025)); CHECK(!oph_range_valid(4095,2));
    for (uint64_t o=0; o<4200; ++o)
        CHECK(oph_range_valid(o,64) == (o <= 4032));
    CHECK(oph_merge_memtype(0,6)==0); CHECK(oph_merge_memtype(6,0)==0);
    CHECK(oph_merge_memtype(4,6)==4); CHECK(oph_merge_memtype(6,4)==4);
    CHECK(oph_merge_memtype(255,6)==6); CHECK(oph_merge_memtype(6,6)==6);
    for (unsigned a=0; a<=6; ++a) for (unsigned b=0; b<=6; ++b)
        CHECK(oph_merge_memtype(a,b)==oph_merge_memtype(b,a));
    setup(); CHECK(oph_walk4(0x1000,0x1234,48,reader,NULL,&t));
    CHECK(t.physical==0xabc234 && t.page_size==4096 && t.user && t.writable && t.executable);
    tables[0][0] &= ~2ULL; tables[2][0] |= 1ULL<<63;
    CHECK(oph_walk4(0x1000,0x1234,48,reader,NULL,&t)); CHECK(!t.writable && !t.executable);
    setup(); tables[3][1]=0; CHECK(!oph_walk4(0x1000,0x1234,48,reader,NULL,&t));
    setup(); tables[2][0]=0x200087; CHECK(oph_walk4(0x1000,0x1234,48,reader,NULL,&t));
    CHECK(t.physical==0x201234 && t.page_size==0x200000);
    tables[2][0] |= 0x1000; CHECK(oph_walk4(0x1000,0x1234,48,reader,NULL,&t)); /* PAT */
    tables[2][0] |= 0x2000; CHECK(!oph_walk4(0x1000,0x1234,48,reader,NULL,&t));
    setup(); tables[1][0]=0x40000087; CHECK(oph_walk4(0x1000,0x1234,48,reader,NULL,&t));
    CHECK(t.physical==0x40001234 && t.page_size==0x40000000);
    setup(); tables[0][0] |= 0x80; CHECK(!oph_walk4(0x1000,0x1234,48,reader,NULL,&t));
    setup(); tables[3][1] |= 1ULL<<49; CHECK(!oph_walk4(0x1000,0x1234,48,reader,NULL,&t));
    CHECK(!oph_walk4(0x1000,0x0000800000000000ULL,48,reader,NULL,&t));
    CHECK(!oph_walk4(0x1000,0x1234,53,reader,NULL,&t));
    CHECK(!oph_walk4(0x9000,0x1234,48,reader,NULL,&t));
    CHECK(!oph_walk4(0x1000,0x1234,48,reader,NULL,NULL));
    printf("PASS: %u ABI, bounds, MTRR and paging checks\n",checks);
    return 0;
}
