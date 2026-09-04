/* SPDX-License-Identifier: MPL-2.0 */
#include <juice/juice.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    juice_config_t config={0};
    juice_agent_t *agent=juice_create(&config);
    if(!agent) return 1;
    char ufrag[258],pwd[258],description[1024];
    memset(ufrag,'u',257);ufrag[257]=0;
    memset(pwd,'p',257);pwd[257]=0;
    int result=juice_set_local_ice_attributes(agent,ufrag,"012345678901234567890123");
    if(result==0) {
        juice_get_local_description(agent,description,sizeof(description));
        printf("COUNTEREXAMPLE: 257-char ufrag accepted; stored ufrag length=%zu\n",strcspn(strstr(description,"a=ice-ufrag:")+12,"\r\n"));
        juice_destroy(agent);return 2;
    }
    ufrag[256]=0;
    if(juice_set_local_ice_attributes(agent,ufrag,pwd)==0) return 3;
    pwd[256]=0;
    if(juice_set_local_ice_attributes(agent,ufrag,pwd)!=0) return 4;
    char remote[1024];
    ufrag[256]='u';
    snprintf(remote,sizeof(remote),"a=ice-ufrag:%s\r\na=ice-pwd:012345678901234567890123\r\n",ufrag);
    if(juice_set_remote_description(agent,remote)==0) return 5;
    juice_destroy(agent);
    puts("ICE credential limits: exact 256 accepted, oversized local/remote rejected PASS");
}
