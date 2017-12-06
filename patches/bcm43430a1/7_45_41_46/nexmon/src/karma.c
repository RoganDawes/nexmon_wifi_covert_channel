#pragma NEXMON targetregion "patch"

#include <firmware_version.h>
#include <wrapper.h>    // wrapper definitions for functions that already exist in the firmware
#include <structs.h>    // structures that are used by the code in the firmware
#include <patcher.h>
#include <helper.h>

//#include "d11.h"
//#include "brcm.h"


#include "karma.h"

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


	//early out if KARMA disabled
	if (!wlc->FW_PAD_UNUSED[0]) return;

	printf("Entered wlc_recv_process_prbreq_hook\n");
	printf("Fw Pad 0x%02x 0x%02x 0x%02x 0x%02x\n", wlc->FW_PAD_UNUSED[0], wlc->FW_PAD_UNUSED[1], wlc->FW_PAD_UNUSED[2], wlc->FW_PAD_UNUSED[3]);
	
	


	if ((ssid = bcm_parse_tlvs(body, body_len, DOT11_MNG_SSID_ID)) != NULL)
	{
		void *p;
		uint8 *pbody;

		//store recieved SSID
		memset(SSID_PRQ, 0, 32);
		memcpy(SSID_PRQ, ssid->data, ssid->len);
		SSID_PRQ_LEN = (*ssid).len;

		printf("Probe Request received for SSID %s\n", SSID_PRQ);

//                bsscfg_hwaddr = wlc_bsscfg_find_by_hwaddr(wlc, &hdr->da);
//                bsscfg = wlc_bsscfg_find_by_bssid(wlc, &hdr->bssid);

		//Use current address as BSSID when searching for BSSCFG
		cfg = wlc_bsscfg_find_by_bssid(wlc, &wlc->pub->cur_etheraddr);

		if (cfg == NULL)
		{
			printf("Invalid bsscfg %p, aborting...", cfg);
			return;
		}
		else printf("Using bsscfg at: %p\n", cfg); //Structs have to be update to fetch index of bsscfg + wlc_if + (bool) _ap

		//backup original SSID
		memcpy(SSID_BSS, cfg->SSID, 32); //Padding 0x00 bytes are already included
		SSID_BSS_LEN = (*cfg).SSID_len;

		printf("PRQ SSID %s (%d), BSS SSID %s (%d)\n", SSID_PRQ, SSID_PRQ_LEN, SSID_BSS, SSID_BSS_LEN);

//print_mem(cfg, 30); //for analysis of bsscfg fields

		len = wlc->pub->bcn_tmpl_len; //should be 512
		printf("bcn_tmpl_len %d\n", len);

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

			printf("PRBRES len %d\nSending Probe Response for %s..\n", len, SSID_PRQ);

			wlc_sendmgmt(wlc, p, wlc->wlcif_list->qi, NULL);

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

	//early out if KARMA disabled
	if (!wlc->FW_PAD_UNUSED[0])
	{
		wlc_recv_mgmt_ctl(wlc, osh, wrxh, p);
		return;
	}

	plcp = PKTDATA(osh, p);

	hdr = (struct dot11_management_header*)(plcp + D11_PHY_HDR_LEN);

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
	
	//early out if KARMA disabled
	if (!bsscfg->wlc->FW_PAD_UNUSED[0])
	{
		wlc_ap_process_assocreq(ap, bsscfg, hdr, body, body_len, scb, short_preamble);
		return;
	}

	printf("wlc_ap_process_assocreq_hook called\n---------------------\n");


	if ((ssid = bcm_parse_tlvs(body + ie_offset, body_len - ie_offset, DOT11_MNG_SSID_ID)) != NULL)
	{
		//Copy SSID of ASSOC
		memset(SSID_ASC, 0, 32);
		memcpy(SSID_ASC, ssid->data, ssid->len);
		SSID_ASC_LEN = (*ssid).len;

		//Copy SSID of BSSCFG SSID
		memcpy(SSID_BSS, bsscfg->SSID, 32); 
		SSID_BSS_LEN = (*bsscfg).SSID_len;

		printf("ASSOC REQ for SSID '%s' (%d), AP has SSID '%s' (%d)\n", SSID_ASC, SSID_ASC_LEN, SSID_BSS, SSID_BSS_LEN);

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

__attribute__((at(0x012da2, "", CHIP_VER_BCM43430a1, FW_VER_7_45_41_46)))
BPatch(wlc_recv_mgmt_ctl_hook, wlc_recv_mgmt_ctl_hook);


__attribute__((at(0x00820b9a, "flashpatch", CHIP_VER_BCM43430a1, FW_VER_7_45_41_46)))
BLPatch(wlc_recv_process_prbreq_hook, wlc_recv_process_prbreq_hook);


//hook the call to wlc_ap_process_assocreq in wlc_recv_mgmt_ctl (0x00820f2e   bl wlc_ap_process_assocreq)
__attribute__((at(0x00820f2e, "flashpatch", CHIP_VER_BCM43430a1, FW_VER_7_45_41_46)))
BLPatch(wlc_ap_process_assocreq_hook, wlc_ap_process_assocreq_hook);



