	#include "globals.h"

#ifdef MODULE_CCCAM
#include "module-cccam.h"
#include "reader-common.h"
#include "module-cccshare.h"

static uint32_t cc_share_id = 0x64;
static LLIST *reported_carddatas;
static CS_MUTEX_LOCK cc_shares_lock;

static int32_t card_added_count = 0;
static int32_t card_removed_count = 0;
static int32_t card_dup_count = 0;
static pthread_t share_updater_thread = 0;

LLIST *get_and_lock_sharelist()
{
		cs_readlock(&cc_shares_lock);
		return reported_carddatas;
}

void unlock_sharelist()
{
		cs_readunlock(&cc_shares_lock);
}

void add_good_bad_sids(struct s_sidtab *ptr, SIDTABBITS sidtabno, struct cc_card *card) {
        //good sids:
        int32_t l;
        for (l=0;l<ptr->num_srvid;l++) {
                struct cc_srvid *srvid;
                cs_malloc(&srvid,sizeof(struct cc_srvid), QUITERROR);
                srvid->sid = ptr->srvid[l];
                srvid->ecmlen = 0; //0=undefined, also not used with "O" CCcam
                if (!ll_contains_data(card->goodsids, srvid, sizeof(struct cc_srvid)))
                	ll_append(card->goodsids, srvid);
        }

        //bad sids:
        if (!sidtabno) return;
        
        struct s_sidtab *ptr_no;
        int32_t n;
        
        for (n=0,ptr_no=cfg.sidtab; ptr_no; ptr_no=ptr_no->next,n++) {
				if (sidtabno&((SIDTABBITS)1<<n)) {
                		int32_t m;
                        int32_t ok_caid = FALSE;
                        for (m=0;m<ptr_no->num_caid;m++) { //search bad sids for this caid:
                        		if (ptr_no->caid[m] == card->caid) {
                                		ok_caid = TRUE;
                                        break;
                                }
                        }
                        if (ok_caid) {
                        		for (l=0;l<ptr_no->num_srvid;l++) {
                                        struct cc_srvid *srvid;
                                        cs_malloc(&srvid,sizeof(struct cc_srvid), QUITERROR);
                                        srvid->sid = ptr_no->srvid[l];
                                        srvid->ecmlen = 0; //0=undefined, also not used with "O" CCcam
                                        if (!ll_contains_data(card->badsids, srvid, sizeof(struct cc_srvid)))
                                        	ll_append(card->badsids, srvid);
                                }
                        }
				}
        }
}

void add_good_bad_sids_by_rdr(struct s_reader *rdr, struct cc_card *card) {

	if (!rdr->sidtabok) return;
	
	struct s_sidtab *ptr;
	int32_t n,i;
	for (n=0,ptr=cfg.sidtab; ptr; ptr=ptr->next,n++) {
		if (rdr->sidtabok&((SIDTABBITS)1<<n)) {
			for (i=0; i<ptr->num_caid;i++) {
				if (ptr->caid[i] == card->caid)
					add_good_bad_sids(ptr, rdr->sidtabno, card);
			}
		}
	}
}


int32_t can_use_ext(struct cc_card *card) {
	if (card->card_type == CT_REMOTECARD)
		return card->is_ext;
		
	if (card->sidtab)
		return (card->sidtab->num_srvid>0);
	else
		return ll_count(card->goodsids) && ll_count(card->badsids);
	return 0;
}

int32_t write_card(struct cc_data *cc, uint8_t *buf, struct cc_card *card, int32_t add_own, int32_t ext, int32_t au_allowed, struct s_client *cl) {
    memset(buf, 0, CC_MAXMSGSIZE);
    buf[0] = card->id >> 24;
    buf[1] = card->id >> 16;
    buf[2] = card->id >> 8;
    buf[3] = card->id & 0xff;
    buf[4] = card->remote_id >> 24;
    buf[5] = card->remote_id >> 16;
    buf[6] = card->remote_id >> 8;
    buf[7] = card->remote_id & 0xFF;
    buf[8] = card->caid >> 8;
    buf[9] = card->caid & 0xff;
    buf[10] = card->hop;
    buf[11] = card->reshare;
    if (au_allowed)
            memcpy(buf + 12, card->hexserial, 8);

    //with cccam 2.2.0 we have assigned and rejected sids:
    int32_t ofs = ext?23:21;

    //write providers:
    LL_ITER it = ll_iter_create(card->providers);
    struct cc_provider *prov;
    while ((prov = ll_iter_next(&it))) {
        uint32_t prid = prov->prov;
        buf[ofs+0] = prid >> 16;
        buf[ofs+1] = prid >> 8;
        buf[ofs+2] = prid & 0xFF;
        if (au_allowed)
        		memcpy(buf + ofs + 3, prov->sa, 4);
        buf[20]++;
        ofs+=7;
    }

    //write sids only if cccam 2.2.x:
    if (ext) {
    	if (card->sidtab) {
		        //good sids:
		        struct s_sidtab *ptr = card->sidtab;
		        int32_t l;
		        for (l=0;l<ptr->num_srvid;l++) {
		            buf[ofs+0] = ptr->srvid[l] >> 8;
		            buf[ofs+1] = ptr->srvid[l] & 0xFF;
		            ofs+=2;
		            buf[21]++; //nassign
		            if (buf[21] >= 240)
		                break;
				}

		        //bad sids:
		        int32_t n;
		        for (n=0,ptr=cfg.sidtab; ptr; ptr=ptr->next,n++) {
						if (cl->sidtabno&((SIDTABBITS)1<<n) || card->sidtabno&((SIDTABBITS)1<<n)) {
                				int32_t m;
                				int32_t ok_caid = FALSE;
                				for (m=0;m<ptr->num_caid;m++) { //search bad sids for this caid:
                        				if (ptr->caid[m] == card->caid) {
                                				ok_caid = TRUE;
                                				break;
                                		}
								}
								if (ok_caid) {
                        				for (l=0;l<ptr->num_srvid;l++) {
                        						buf[ofs+0] = ptr->srvid[l] >> 8;
                        						buf[ofs+1] = ptr->srvid[l] & 0xFF;
                        						ofs+=2;
                        						buf[22]++; //nreject
                        						if (buf[22] >= 240)
														break;
		                                }
        		                }
						}
						if (buf[22] >= 240)
							break;
				}
				
    	} else {
		        //assigned sids:
		        it = ll_iter_create(card->goodsids);
		        struct cc_srvid *srvid;
		        while ((srvid = ll_iter_next(&it))) {
		            buf[ofs+0] = srvid->sid >> 8;
		            buf[ofs+1] = srvid->sid & 0xFF;
		            ofs+=2;
		            buf[21]++; //nassign
		            if (buf[21] >= 200)
		                break;
		        }
		
		        //reject sids:
		        it = ll_iter_create(card->badsids);
		        while ((srvid = ll_iter_next(&it))) {
		            buf[ofs+0] = srvid->sid >> 8;
		            buf[ofs+1] = srvid->sid & 0xFF;
		            ofs+=2;
		            buf[22]++; //nreject
		            if (buf[22] >= 200)
		                break;
		        }
		}
    }

    //write remote nodes
    int32_t nremote_ofs = ofs;
    ofs++;
    it = ll_iter_create(card->remote_nodes);
    uint8_t *remote_node;
    while ((remote_node = ll_iter_next(&it))) {
        memcpy(buf+ofs, remote_node, 8);
        ofs+=8;
        buf[nremote_ofs]++;
    }
    if (add_own) {
        memcpy(buf+ofs, cc->node_id, 8);
        ofs+=8;
        buf[nremote_ofs]++;
    }
    return ofs;
}


int32_t send_card_to_clients(struct cc_card *card, struct s_client *one_client) {
        int32_t count = 0;

        uint8_t buf[CC_MAXMSGSIZE];

        struct s_client *cl;
        for (cl = one_client?one_client:first_client; cl; cl=one_client?NULL:cl->next) {
                struct cc_data *cc = cl->cc;
                if (cl->typ=='c' && cc && ((one_client && cc->mode != CCCAM_MODE_SHUTDOWN) || (ph[cl->ctyp].num == R_CCCAM && cc->mode == CCCAM_MODE_NORMAL))) { //CCCam-Client!
                		int32_t is_ext = cc->cccam220 && can_use_ext(card);
                		int32_t msg = is_ext?MSG_NEW_CARD_SIDINFO:MSG_NEW_CARD;
                        if (card_valid_for_client(cl, card)) {
								int32_t usr_reshare = cl->account->cccreshare;
								if (usr_reshare == -1) usr_reshare = cfg.cc_reshare;
                                int32_t usr_ignorereshare = cl->account->cccignorereshare;
                                if (usr_ignorereshare == -1) usr_ignorereshare = cfg.cc_ignore_reshare;
                                
                                int32_t reader_reshare = card->origin_reader?card->rdr_reshare:usr_reshare;
                                if (reader_reshare == -1) reader_reshare = cfg.cc_reshare;
                                int32_t reshare = (reader_reshare < usr_reshare) ? reader_reshare : usr_reshare;
								int32_t new_reshare;
								if (card->card_type == CT_CARD_BY_SERVICE_USER)
									new_reshare = usr_reshare;
								else if (cfg.cc_ignore_reshare || usr_ignorereshare)
									new_reshare = reshare;
								else {
									new_reshare = card->reshare;
									if (card->card_type == CT_REMOTECARD)
										new_reshare--;
									if (new_reshare > reshare)
										new_reshare = reshare;
								}
                                if (new_reshare < 0)
                                		continue;

								if (!card->id)
										card->id = cc_share_id++;

								int32_t len = write_card(cc, buf, card, 1, is_ext, ll_count(cl->aureader_list), cl);
								//buf[10] = card->hop-1;
								buf[11] = new_reshare;

								if (cc_cmd_send(cl, buf, len, msg) < 0)
										cc->mode = CCCAM_MODE_SHUTDOWN;
								count++;
                        }
                }
		}
        return count;
}

void send_remove_card_to_clients(struct cc_card *card) {
		if (!card || !card->id)
				return;
				
		uint8_t buf[4];
		buf[0] = card->id >> 24;
		buf[1] = card->id >> 16;
		buf[2] = card->id >> 8;
		buf[3] = card->id & 0xFF;

		struct s_client *cl;
		for (cl = first_client; cl; cl=cl->next) {
				struct cc_data *cc = cl->cc;
				if (cl->typ=='c' && cc && ph[cl->ctyp].num == R_CCCAM && cc->mode == CCCAM_MODE_NORMAL) { //CCCam-Client!
						if (card_valid_for_client(cl, card)) {
								cc_cmd_send(cl, buf, 4, MSG_CARD_REMOVED);
						}
				}
		}
}


/**
 * if idents defined on an cccam reader, the cards caid+provider are checked.
 * return 1 a) if no ident defined b) card is in identlist
 *        0 if card is not in identlist
 *
 * a card is in the identlist, if the cards caid is matching and mininum a provider is matching
 **/
int32_t chk_ident(FTAB *ftab, struct cc_card *card) {

    int32_t j, k;
    int32_t res = 1;

    if (ftab && ftab->filts) {
        for (j = 0; j < ftab->nfilts; j++) {
            if (ftab->filts[j].caid) {
                res = 0;
                if (ftab->filts[j].caid==card->caid) { //caid matches!

                    int32_t nprids = ftab->filts[j].nprids;
                    if (!nprids) // No Provider ->Ok
                        return 1;


                    LL_ITER it = ll_iter_create(card->providers);
                    struct cc_provider *prov;

                    while ((prov = ll_iter_next(&it))) {
                        for (k = 0; k < nprids; k++) {
                            uint32_t prid = ftab->filts[j].prids[k];
                            if (prid == prov->prov) { //Provider matches
                                return 1;
                            }
                        }
                    }
                }
            }
        }
    }
    return res;
}

int32_t cc_clear_reported_carddata(LLIST *reported_carddatas, LLIST *except,
                int32_t send_removed) {
        int32_t i=0;
        LL_ITER it = ll_iter_create(reported_carddatas);
        struct cc_card *card;
        while ((card = ll_iter_next(&it))) {
                struct cc_card *card2 = NULL;
                if (except) {
                        LL_ITER it2 = ll_iter_create(except);
                        while ((card2 = ll_iter_next(&it2))) {
                                if (card == card2)
                                        break;
                        }
                }

                if (!card2 && ll_iter_remove(&it)) { //check result of ll_iter_remove, because another thread could removed it
                        if (send_removed) {
                        		cs_debug_mask(D_TRACE, "s-card removed: id %8X remoteid %8X caid %4X hop %d reshare %d originid %8X cardtype %d", 
									card->id, card->remote_id, card->caid, card->hop, card->reshare, card->origin_id, card->card_type);

                        		send_remove_card_to_clients(card);
						}
                        cc_free_card(card);
                        i++;
                }
        }
        return i;
}

int32_t cc_free_reported_carddata(LLIST *reported_carddatas, LLIST *except,
                int32_t send_removed) {
        int32_t i=0;
        if (reported_carddatas) {
                i = cc_clear_reported_carddata(reported_carddatas, except, send_removed);
                ll_destroy(reported_carddatas);
        }
        return i;
}

int32_t card_valid_for_client(struct s_client *cl, struct cc_card *card) {

        //Check group:
        if (card->grp && !(card->grp & cl->grp))
                return 0;

        if (!chk_ident(&cl->ftab, card))
                return 0;

        //Check caids:
        if (!chk_ctab(card->caid, &cl->ctab))
                return 0;

        //Check reshare
        if (card->card_type == CT_REMOTECARD) {
        	int32_t usr_ignorereshare = cl->account->cccignorereshare;
        	if (usr_ignorereshare == -1) usr_ignorereshare = cfg.cc_ignore_reshare;
        	if (!cfg.cc_ignore_reshare && !usr_ignorereshare && !card->reshare)
        		return 0;
		}
        		
		//Check account maxhops:
		if (cl->account->cccmaxhops < card->hop)
				return 0;

		//Check remote node id, if card is from there, ignore it!
        LL_ITER it = ll_iter_create(card->remote_nodes);
		uint8_t * node;
		struct cc_data *cc = cl->cc;
        while ((node=ll_iter_next(&it))) {
        		if (!memcmp(node, cc->peer_node_id, 8)) {
        				return 0;
				}
		}

        //Check Services:
        if (ll_count(card->providers)) {
        	it = ll_iter_create(card->providers);
        	struct cc_provider *prov;
        	int8_t found=0;
        	while ((prov = ll_iter_next(&it))) {
        		uint32_t prid = prov->prov;
                if (chk_srvid_by_caid_prov(cl, card->caid, prid)) {
                	found = 1;
                	break;
				}
			}
			if (!found) return 0;
		}
		
        //Check Card created by Service:
        if (card->sidtab) {
        		struct s_sidtab *ptr;
        		int32_t j;
        		int32_t ok = !cl->sidtabok && !cl->sidtabno; //default valid if no positive services and no negative services
        		if (!ok) {
        				if (!cl->sidtabok) // no positive services, so ok by default if no negative found
        						ok=1;
        						
		        		for (j=0,ptr=cfg.sidtab; ptr; ptr=ptr->next,j++) {
        						if (ptr == card->sidtab) {
										if (cl->sidtabno&((SIDTABBITS)1<<j))
        										return 0;
										if (cl->sidtabok&((SIDTABBITS)1<<j))
        										ok = 1;
										break;
								}
						}
                }
                if (!ok)
                		return 0;
		}
                        				
        return 1;
}

uint32_t get_reader_prid(struct s_reader *rdr, int32_t j) {
    return b2i(3, &rdr->prid[j][1]);
}
//uint32_t get_reader_prid(struct s_reader *rdr, int32_t j) {
//  uint32_t prid;
//  if (!(rdr->typ & R_IS_CASCADING)) { // Real cardreaders have 4-byte Providers
//      prid = b2i(4, &rdr->prid[j][0]);
//      //prid = (rdr->prid[j][0] << 24) | (rdr->prid[j][1] << 16)
//      //      | (rdr->prid[j][2] << 8) | (rdr->prid[j][3] & 0xFF);
//  } else { // Cascading/Network-reader 3-bytes Providers
//      prid = b2i(3, &rdr->prid[j][0]);
//      //prid = (rdr->prid[j][0] << 16) | (rdr->prid[j][1] << 8)
//      //      | (rdr->prid[j][2] & 0xFF);
//
//  }
//  return prid;
//}

void copy_sids(LLIST *dst, LLIST *src) {
    LL_ITER it_src = ll_iter_create(src);
    LL_ITER it_dst = ll_iter_create(dst);
    struct cc_srvid *srvid_src;
    struct cc_srvid *srvid_dst;
    while ((srvid_src=ll_iter_next(&it_src))) {
        ll_iter_reset(&it_dst);
        while ((srvid_dst=ll_iter_next(&it_dst))) {
            if (sid_eq(srvid_src, srvid_dst))
                break;
        }
        if (!srvid_dst) {
            srvid_dst = cs_malloc(&srvid_dst, sizeof(struct cc_srvid), QUITERROR);
            memcpy(srvid_dst, srvid_src, sizeof(struct cc_srvid));
            ll_iter_insert(&it_dst, srvid_dst);
        }
    }
}


int32_t add_card_providers(struct cc_card *dest_card, struct cc_card *card,
        int32_t copy_remote_nodes) {
    int32_t modified = 0;

    //1. Copy nonexisting providers, ignore double:
    struct cc_provider *prov_info;
    LL_ITER it_src = ll_iter_create(card->providers);
    LL_ITER it_dst = ll_iter_create(dest_card->providers);

    struct cc_provider *provider;
    while ((provider = ll_iter_next(&it_src))) {
        ll_iter_reset(&it_dst);
        while ((prov_info = ll_iter_next(&it_dst))) {
            if (prov_info->prov == provider->prov)
                break;
        }
        if (!prov_info) {
            struct cc_provider *prov_new = cs_malloc(&prov_new, sizeof(struct cc_provider), QUITERROR);
            memcpy(prov_new, provider, sizeof(struct cc_provider));
            ll_iter_insert(&it_dst, prov_new);
            modified = 1;
        }
    }

    if (copy_remote_nodes) {
        //2. Copy nonexisting remote_nodes, ignoring existing:
        it_src = ll_iter_create(card->remote_nodes);
        it_dst = ll_iter_create(dest_card->remote_nodes);
        uint8_t *remote_node;
        uint8_t *remote_node2;
        while ((remote_node = ll_iter_next(&it_src))) {
            ll_iter_reset(&it_dst);
            while ((remote_node2 = ll_iter_next(&it_dst))) {
                if (memcmp(remote_node, remote_node2, 8) == 0)
                    break;
            }
            if (!remote_node2) {
                uint8_t* remote_node_new = cs_malloc(&remote_node_new, 8, QUITERROR);
                memcpy(remote_node_new, remote_node, 8);
                ll_iter_insert(&it_dst, remote_node_new);
                modified = 1;
            }
        }
    }
    return modified;
}

#define TIMEOUT_SECONDS 3600

void set_card_timeout(struct cc_card *card)
{
	card->timeout = time(NULL)+TIMEOUT_SECONDS+(fast_rnd()-128)*2;
}
    
struct cc_card *create_card(struct cc_card *card) {
    struct cc_card *card2 = cs_malloc(&card2, sizeof(struct cc_card), QUITERROR);
    if (card)
        memcpy(card2, card, sizeof(struct cc_card));
    else
        memset(card2, 0, sizeof(struct cc_card));
    card2->providers = ll_create();
    card2->badsids = ll_create();
    card2->goodsids = ll_create();
    card2->remote_nodes = ll_create();

    if (card) {
        copy_sids(card2->goodsids, card->goodsids);
        copy_sids(card2->badsids, card->badsids);
        card2->id = 0;
    }
    else
    	set_card_timeout(card2);

    return card2;
}

struct cc_card *create_card2(struct s_reader *rdr, int32_t j, uint16_t caid, uint8_t reshare) {

    struct cc_card *card = create_card(NULL);
    card->remote_id = (rdr?(rdr->cc_id << 16):0x7F7F8000)|j;
    card->caid = caid;
    card->reshare = reshare;
    card->origin_reader = rdr;
    if (rdr) {
    	card->grp = rdr->grp;
    	card->rdr_reshare = rdr->cc_reshare; //copy reshare because reader could go offline
    	card->sidtabno = rdr->sidtabno;
    	card->hop = rdr->cc_hop;
	}
	else card->rdr_reshare = reshare;
    return card;
}

/**
 * num_same_providers checks if card1 has exactly the same providers as card2
 * returns same provider count
 **/
int32_t num_same_providers(struct cc_card *card1, struct cc_card *card2) {

    int32_t found=0;

    LL_ITER it1 = ll_iter_create(card1->providers);
    LL_ITER it2 = ll_iter_create(card2->providers);

    struct cc_provider *prov1, *prov2;

    while ((prov1=ll_iter_next(&it1))) {

        ll_iter_reset(&it2);
        while ((prov2=ll_iter_next(&it2))) {
            if (prov1->prov==prov2->prov) {
                found++;
                break;
            }

        }
    }
    return found;
}

/**
 * equal_providers checks if card1 has exactly the same providers as card2
 * returns 1=equal 0=different
 **/
int32_t equal_providers(struct cc_card *card1, struct cc_card *card2) {

    if (ll_count(card1->providers) != ll_count(card2->providers))
    	return 0;
    if (ll_count(card1->providers) == 0)
       return 1;

    LL_ITER it1 = ll_iter_create(card1->providers);
    LL_ITER it2 = ll_iter_create(card2->providers);

    struct cc_provider *prov1, *prov2;

    while ((prov1=ll_iter_next(&it1))) {

        ll_iter_reset(&it2);
        while ((prov2=ll_iter_next(&it2))) {
            if (prov1->prov==prov2->prov) {
                break;
            }

        }
        if (!prov2) break;
    }
    return (prov1 == NULL);
}

/**
 * Adds a new card to a cardlist.
 */
int32_t add_card_to_serverlist(LLIST *cardlist, struct cc_card *card, int free_card) {

    int32_t modified = 0;
    LL_ITER it = ll_iter_create(cardlist);
    struct cc_card *card2;

    //Minimize all, transmit just CAID, merge providers:
    if (cfg.cc_minimize_cards == MINIMIZE_CAID && !cfg.cc_forward_origin_card) {
        while ((card2 = ll_iter_next(&it))) {
        	//compare caid, hexserial, cardtype and sidtab (if any):
            if (same_card2(card, card2)) {
                //Merge cards only if resulting providercount is smaller than CS_MAXPROV
                int32_t nsame, ndiff, nnew;

                nsame = num_same_providers(card, card2); //count same cards
                ndiff = ll_count(card->providers)-nsame; //cound different cards, this cound will be added
                nnew = ndiff + ll_count(card2->providers); //new card count after add. because its limited to CS_MAXPROV, dont add it

                if (nnew <= CS_MAXPROV)
                    break;
            }
		}
		
        if (!card2) { //Not found->add it:
        	if (free_card) { //Use this card
        		free_card = FALSE;
        		ll_iter_insert(&it, card);
			} else {
            	card2 = create_card(card); //Copy card
            	card2->hop = 0;
			    ll_iter_insert(&it, card2);
			    add_card_providers(card2, card, 1); //copy providers to new card. Copy remote nodes to new card
			}
            modified = 1;

        } else { //found, merge providers:
			card_dup_count++;
        	add_card_providers(card2, card, 0); //merge all providers
        	ll_clear_data(card2->remote_nodes); //clear remote nodes
           	if (!card2->sidtab)
           		ll_clear_data(card2->badsids);
		}
    }

    //Removed duplicate cards, keeping card with lower hop:
    else if (cfg.cc_minimize_cards == MINIMIZE_HOPS && !cfg.cc_forward_origin_card) {
        while ((card2 = ll_iter_next(&it))) {
        	//compare caid, hexserial, cardtype, sidtab (if any), providers:
            if (same_card2(card, card2) && equal_providers(card, card2)) {
                break;
            }
        }
        
        if (card2 && card2->hop > card->hop) { //hop is smaller, drop old card
            ll_iter_remove(&it);
            cc_free_card(card2);
            card2 = NULL;
            card_dup_count++;
        }

        if (!card2) { //Not found->add it:
        	if (free_card) { //use this card
        		free_card = FALSE;
        		ll_iter_insert(&it, card);
			} else { 
            	card2 = create_card(card); //copy card
            	ll_iter_insert(&it, card2);
            	add_card_providers(card2, card, 1); //copy providers to new card. Copy remote nodes to new card
			}
            modified = 1;
        } else { //found, merge cards (providers are same!)
        	card_dup_count++;
        	add_card_providers(card2, card, 0);
           	if (!card2->sidtab)
           		ll_clear_data(card2->badsids);
		}

    }
    //like cccam:
    else { //just remove duplicate cards (same ids)
        while ((card2 = ll_iter_next(&it))) {
        	//compare remote_id, first_node, caid, hexserial, cardtype, sidtab (if any), providers:
            if (same_card(card, card2))
                break;
        }
        
        if (card2 && card2->hop > card->hop) { //same card, if hop greater drop card
            ll_iter_remove(&it);
            cc_free_card(card2);
            card2 = NULL;
            card_dup_count++;
        }
        if (!card2) { //Not found, add it:
        	if (free_card) {
        		free_card = FALSE;
        		ll_iter_insert(&it, card);
        	} else {
            	card2 = create_card(card);
            	ll_iter_insert(&it, card2);
            	add_card_providers(card2, card, 1);
			}
            modified = 1;
        } else { //Found, everything is same (including providers)
        	card_dup_count++;
		}
    }
    
    if (free_card)
    	cc_free_card(card);
    	
    return modified;
}

/**
 * returns true if timeout-time is reached
 * only local cards needs to be renewed after 1h. "O" CCCam throws away cards older than 1,2h
 **/
int32_t card_timed_out(struct cc_card *card)
{
	//!=CT_REMOTECARD = LOCALCARD (or virtual cards by caid/ident/service)
	//timeout is set in future, so if current time is bigger, timeout is reached
    int32_t res = (card->card_type != CT_REMOTECARD) && (card->timeout < time(NULL)); //local card is older than 1h?
    if (res)
		cs_debug_mask(D_TRACE, "card %08X timed out! refresh forced", card->id?card->id:card->origin_id);
    return res;
}
            
/**
 * returns true if card1 is already reported.
 * "reported" means, we already have this card1 in our sharelist.
 * if the card1 is already reported, we throw it away, because we build a new sharelist
 * so after finding all reported cards, we have a list of reported cards, which aren't used anymore
 **/
int32_t find_reported_card(struct cc_card *card1)
{
    LL_ITER it = ll_iter_create(reported_carddatas);
    struct cc_card *card2;
    while ((card2 = ll_iter_next(&it))) {
        if (same_card(card1, card2) && !card_timed_out(card2)) {
            card1->id = card2->id; //Set old id !!
            card1->timeout = card2->timeout;
            cc_free_card(card2);
            ll_iter_remove(&it);
            return 1; //Old card and new card are equal!
        }
    }
    return 0; //Card not found
}

/**
* Server:
* Adds a cccam-carddata buffer to the list of reported carddatas
*/
void cc_add_reported_carddata(LLIST *reported_carddatas, struct cc_card *card) {
		ll_append(reported_carddatas, card);
}       
       
/**
 * adds the card to the list of the new reported carddatas - this is the new sharelist
 * if this card is not already reported, we send them to the clients
 * if this card is already reported, find_reported_card throws the "origin" card away
 * so the "old" sharelist is reduced
 **/
void report_card(struct cc_card *card, LLIST *new_reported_carddatas, LLIST *new_cards)
{
    if (!find_reported_card(card)) { //Add new card:
    	
    	cs_debug_mask(D_TRACE, "s-card added: id %8X remoteid %8X caid %4X hop %d reshare %d originid %8X cardtype %d", 
    		card->id, card->remote_id, card->caid, card->hop, card->reshare, card->origin_id, card->card_type);
    		
        ll_append(new_cards, card);

        card_added_count++;
    }
    cc_add_reported_carddata(new_reported_carddatas, card);
}


/**
 * Server:
 * Reports all caid/providers to the connected clients
 * returns 1=ok, 0=error
 * cfg.cc_reshare_services 	=0 CCCAM reader reshares only received cards + defined reader services
 *				=1 CCCAM reader reshares received cards + defined services
 *				=2 CCCAM reader reshares only defined reader-services as virtual cards
 *				=3 CCCAM reader reshares only defined user-services as virtual cards
 *				=4 CCCAM reader reshares only received cards
 */
void update_card_list() {
	cs_writelock(&cc_shares_lock);
    int32_t j, flt;

    LLIST *server_cards = ll_create();
    LLIST *new_reported_carddatas = ll_create();

    card_added_count = 0;
    card_removed_count = 0;
    card_dup_count = 0;

    //User-Services:
    if (cfg.cc_reshare_services==3 && cfg.sidtab) {
        struct s_sidtab *ptr;
        for (j=0,ptr=cfg.sidtab; ptr; ptr=ptr->next,j++) {
                int32_t k;
                for (k=0;k<ptr->num_caid;k++) {
                    struct cc_card *card = create_card2(NULL, (j<<8)|k, ptr->caid[k], cfg.cc_reshare);
                    card->card_type = CT_CARD_BY_SERVICE_USER;
                    card->sidtab = ptr;
                    int32_t l;
                    for (l=0;l<ptr->num_provid;l++) {
                        struct cc_provider *prov = cs_malloc(&prov, sizeof(struct cc_provider), QUITERROR);
                        memset(prov, 0, sizeof(struct cc_provider));
                        prov->prov = ptr->provid[l];
                        ll_append(card->providers, prov);
                    }

                    add_card_to_serverlist(server_cards, card, TRUE);
                }
                flt=1;
        }
    }
    else
    {
        struct s_reader *rdr;
        int32_t r = 0;
        for (rdr = first_active_reader; rdr; rdr = rdr->next) {
            //Generate a uniq reader id:
            if (!rdr->cc_id) {
                rdr->cc_id = ++r;
                struct s_reader *rdr2;
                for (rdr2 = first_active_reader; rdr2; rdr2 = rdr2->next) {
                    if (rdr2 != rdr && rdr2->cc_id == rdr->cc_id) {
                        rdr2 = first_active_reader;
                        rdr->cc_id=++r;
                    }
                }
            }

            flt = 0;

            int32_t reshare = rdr->cc_reshare;
            if (reshare == -1) reshare = cfg.cc_reshare;
            
            //Reader-Services:
            if ((cfg.cc_reshare_services==1||cfg.cc_reshare_services==2||(!rdr->caid && rdr->typ != R_CCCAM && cfg.cc_reshare_services!=4 )) && 
            		cfg.sidtab && (rdr->sidtabno || rdr->sidtabok)) {
                struct s_sidtab *ptr;
                for (j=0,ptr=cfg.sidtab; ptr; ptr=ptr->next,j++) {
                    if (!(rdr->sidtabno&((SIDTABBITS)1<<j)) && (rdr->sidtabok&((SIDTABBITS)1<<j))) {
                        int32_t k;
                        for (k=0;k<ptr->num_caid;k++) {
                            struct cc_card *card = create_card2(rdr, (j<<8)|k, ptr->caid[k], reshare);
                            card->card_type = CT_CARD_BY_SERVICE_READER;
                            card->sidtab = ptr;
                            int32_t l;
                            for (l=0;l<ptr->num_provid;l++) {
                                struct cc_provider *prov = cs_malloc(&prov, sizeof(struct cc_provider), QUITERROR);
                                prov->prov = ptr->provid[l];
                                ll_append(card->providers, prov);
                            }

                            if (chk_ident(&rdr->ftab, card) && chk_ctab(card->caid, &rdr->ctab)) {
                            	if (!rdr->audisabled)
									cc_UA_oscam2cccam(rdr->hexserial, card->hexserial, card->caid);
                        
	                            add_card_to_serverlist(server_cards, card, TRUE);
	                    	    flt=1;
							}
							else 
								cc_free_card(card);
						}
                    }
                }
            }

            //Filts by Hardware readers:
            if ((rdr->typ != R_CCCAM) && rdr->ftab.filts && !flt) {
                for (j = 0; j < CS_MAXFILTERS; j++) {
                	uint16_t caid = rdr->ftab.filts[j].caid;
                    if (caid) {
                        struct cc_card *card = create_card2(rdr, j, caid, reshare);
                        card->card_type = CT_LOCALCARD;
                        
                        //Setting UA: (Unique Address):
                        if (!rdr->audisabled)
								cc_UA_oscam2cccam(rdr->hexserial, card->hexserial, caid);
                        //cs_log("Ident CCcam card report caid: %04X readr %s subid: %06X", rdr->ftab.filts[j].caid, rdr->label, rdr->cc_id);
                        int32_t k;
                        for (k = 0; k < rdr->ftab.filts[j].nprids; k++) {
                            struct cc_provider *prov = cs_malloc(&prov, sizeof(struct cc_provider), QUITERROR);
                            prov->prov = rdr->ftab.filts[j].prids[k];

                            //cs_log("Ident CCcam card report provider: %02X%02X%02X", buf[21 + (k*7)]<<16, buf[22 + (k*7)], buf[23 + (k*7)]);
                            if (!rdr->audisabled) {
                                int32_t l;
                                for (l = 0; l < rdr->nprov; l++) {
                                    uint32_t rprid = get_reader_prid(rdr, l);
                                    if (rprid == prov->prov)
                                        cc_SA_oscam2cccam(&rdr->sa[l][0], prov->sa);
                                }
                            }

                            ll_append(card->providers, prov);
                        }

                        add_good_bad_sids_by_rdr(rdr, card);
						add_card_to_serverlist(server_cards, card, TRUE);
                        flt = 1;
                    }
                }
            }

            if ((rdr->typ != R_CCCAM) && !rdr->caid && !flt) {
                for (j = 0; j < CS_MAXCAIDTAB; j++) {
                    //cs_log("CAID map CCcam card report caid: %04X cmap: %04X", rdr->ctab.caid[j], rdr->ctab.cmap[j]);
                    uint16_t lcaid = rdr->ctab.caid[j];

                    if (!lcaid || (lcaid == 0xFFFF))
                        lcaid = rdr->ctab.cmap[j];

                    if (lcaid && (lcaid != 0xFFFF)) {
                        struct cc_card *card = create_card2(rdr, j, lcaid, reshare);
                        card->card_type = CT_CARD_BY_CAID1;
                        if (!rdr->audisabled)
                            cc_UA_oscam2cccam(rdr->hexserial, card->hexserial, lcaid);

						add_good_bad_sids_by_rdr(rdr, card);
                        add_card_to_serverlist(server_cards, card, TRUE);
                        flt = 1;
                    }
                }
            }

            if ((rdr->typ != R_CCCAM) && rdr->ctab.caid[0] && !flt) {
                //cs_log("tcp_connected: %d card_status: %d ", rdr->tcp_connected, rdr->card_status);
                int32_t c;
   		        if (rdr->tcp_connected || rdr->card_status == CARD_INSERTED) {
	                for (c=0;c<CS_MAXCAIDTAB;c++) 
	                {
						uint16_t caid = rdr->ctab.caid[c];
						if (!caid) break;
						
						struct cc_card *card = create_card2(rdr, c, caid, reshare);
						card->card_type = CT_CARD_BY_CAID2;
	                
	    		        if (!rdr->audisabled)
	    		        	cc_UA_oscam2cccam(rdr->hexserial, card->hexserial, caid);
						for (j = 0; j < rdr->nprov; j++) {
	        	        	uint32_t prid = get_reader_prid(rdr, j);
                		    struct cc_provider *prov = cs_malloc(&prov, sizeof(struct cc_provider), QUITERROR);
		                    prov->prov = prid;
		                    //cs_log("Ident CCcam card report provider: %02X%02X%02X", buf[21 + (k*7)]<<16, buf[22 + (k*7)], buf[23 + (k*7)]);
			                if (!rdr->audisabled) {
			                    //Setting SA (Shared Addresses):
			                    cc_SA_oscam2cccam(rdr->sa[j], prov->sa);
			                }
			            	ll_append(card->providers, prov);
		                    //cs_log("Main CCcam card report provider: %02X%02X%02X%02X", buf[21+(j*7)], buf[22+(j*7)], buf[23+(j*7)], buf[24+(j*7)]);
		                }
		                add_good_bad_sids_by_rdr(rdr, card);
						add_card_to_serverlist(server_cards, card, TRUE);
						flt = 1;
					}
				}
            }


            if ((rdr->typ != R_CCCAM) && rdr->caid && !flt) {
                //cs_log("tcp_connected: %d card_status: %d ", rdr->tcp_connected, rdr->card_status);
                if (rdr->tcp_connected || rdr->card_status == CARD_INSERTED) {
                	uint16_t caid = rdr->caid;
                	struct cc_card *card = create_card2(rdr, 1, caid, reshare);
                	card->card_type = CT_CARD_BY_CAID3;
                
	                if (!rdr->audisabled)
	                    cc_UA_oscam2cccam(rdr->hexserial, card->hexserial, caid);
	                for (j = 0; j < rdr->nprov; j++) {
	                    uint32_t prid = get_reader_prid(rdr, j);
	                    struct cc_provider *prov = cs_malloc(&prov, sizeof(struct cc_provider), QUITERROR);
	                    prov->prov = prid;
	                    //cs_log("Ident CCcam card report provider: %02X%02X%02X", buf[21 + (k*7)]<<16, buf[22 + (k*7)], buf[23 + (k*7)]);
	                    if (!rdr->audisabled) {
	                        //Setting SA (Shared Addresses):
	                        cc_SA_oscam2cccam(rdr->sa[j], prov->sa);
	                    }
	                    ll_append(card->providers, prov);
	                    //cs_log("Main CCcam card report provider: %02X%02X%02X%02X", buf[21+(j*7)], buf[22+(j*7)], buf[23+(j*7)], buf[24+(j*7)]);
	                }
	                add_good_bad_sids_by_rdr(rdr, card);
                    add_card_to_serverlist(server_cards, card, TRUE);
				}
            }

            if (rdr->typ == R_CCCAM && (cfg.cc_reshare_services<2 || cfg.cc_reshare_services==4) && rdr->card_status != CARD_FAILURE) {

                cs_debug_mask(D_TRACE, "asking reader %s for cards...", rdr->label);

                struct cc_card *card;
                struct s_client *rc = rdr->client;
                struct cc_data *rcc = rc?rc->cc:NULL;

                int32_t count = 0;
                if (rcc && rcc->cards && rcc->mode == CCCAM_MODE_NORMAL) {
                	cs_readlock(&rcc->cards_busy);
				
              		LL_ITER it = ll_iter_create(rcc->cards);
                    while ((card = ll_iter_next(&it))) {
                    	if (chk_ctab(card->caid, &rdr->ctab)) {
                        	int32_t ignore = 0;

                            LL_ITER it2 = ll_iter_create(card->providers);
                            struct cc_provider *prov;
                            while ((prov = ll_iter_next(&it2))) {
                    			uint32_t prid = prov->prov;
                                if (!chk_srvid_by_caid_prov(rdr->client, card->caid, prid)) {
                                	ignore = 1;
                                    break;
								}
							}

                            if (!ignore) { //Filtered by service
                            	add_card_to_serverlist(server_cards, card, FALSE);
                                count++;
							}
						}
					}
                    cs_readunlock(&rcc->cards_busy);
                }
                else
                	cs_debug_mask(D_TRACE, "reader %s not active! (mode=%d)", rdr->label, rcc?rcc->mode:-1);
                cs_debug_mask(D_TRACE, "got %d cards from %s", count, rdr->label);
            }
        }
    }

    LLIST *new_cards = ll_create(); //List of new (added) cards
    
    //report reshare cards:
    //cs_debug_mask(D_TRACE, "%s reporting %d cards", getprefix(), ll_count(server_cards));
    LL_ITER it = ll_iter_create(server_cards);
    
    //we compare every card of our new list (server_cards) with the last list.
    struct cc_card *card;
    while ((card = ll_iter_next(&it))) {
            //cs_debug_mask(D_TRACE, "%s card %d caid %04X hop %d", getprefix(), card->id, card->caid, card->hop);

            report_card(card, new_reported_carddatas, new_cards);
            ll_iter_remove(&it);
    }
    cc_free_cardlist(server_cards, TRUE);

    //remove unsed, remaining cards:
    card_removed_count += cc_free_reported_carddata(reported_carddatas, new_reported_carddatas, TRUE);
    reported_carddatas = new_reported_carddatas;
    
    //now send new cards. Always remove first, then add new:
    it = ll_iter_create(new_cards);
    while ((card = ll_iter_next(&it))) {
		send_card_to_clients(card, NULL);
	}
	ll_destroy(new_cards);

    
    cs_writeunlock(&cc_shares_lock);

	cs_debug_mask(D_TRACE, "reported/updated +%d/-%d/dup %d of %d cards to sharelist",
       		card_added_count, card_removed_count, card_dup_count, ll_count(reported_carddatas));
}

int32_t cc_srv_report_cards(struct s_client *cl) {

	cs_readlock(&cc_shares_lock);
	LLIST *carddata;
	struct cc_data *cc = cl->cc;
	struct cc_card *card;
	carddata = reported_carddatas;		// sending carddata sometimes takes longer and the static llist may get cleaned and recreated while that
	LL_ITER it = ll_iter_create(carddata);
	while (cl->cc && cc->mode != CCCAM_MODE_SHUTDOWN && carddata == reported_carddatas && (card = ll_iter_next(&it))) {
		send_card_to_clients(card, cl);
	}
	cs_readunlock(&cc_shares_lock);
	
	return cl->cc && cc->mode != CCCAM_MODE_SHUTDOWN;
}

void refresh_shares()
{
		update_card_list();
}

#define DEFAULT_INTERVAL 30

void share_updater()
{
		int32_t i = DEFAULT_INTERVAL + cfg.waitforcards_extra_delay/1000;
		uint32_t last_check = 0;
		uint32_t last_card_check = 0;
		uint32_t card_count = 0;
		while (TRUE) {
				if ((i > 0 || cfg.cc_forward_origin_card) && card_count < 100) { //fast refresh only if we have less cards
						cs_debug_mask(D_TRACE, "share-updater mode=initfast t=1s i=%d", i);
						cs_sleepms(1000);
						i--;
				}
				else if (i > 0) {
						cs_debug_mask(D_TRACE, "share-updater mode=initslow t=6s i=%d", i);
						cs_sleepms(6000); //1s later than garbage collector because this list uses much space
						i-=6;
				}
				else
				{
						if (cfg.cc_update_interval <= 10)
								cfg.cc_update_interval = DEFAULT_UPDATEINTERVAL;
						cs_debug_mask(D_TRACE, "share-updater mode=interval t=%ds", cfg.cc_update_interval);
						cs_sleepms(cfg.cc_update_interval*1000);
				}
				
				uint32_t cur_check = 0;
				uint32_t cur_card_check = 0;
				card_count = 0;
				struct s_reader *rdr;
				struct cc_data *cc;
				for (rdr=first_active_reader; rdr; rdr=rdr->next) {
						if (rdr->client && (cc=rdr->client->cc)) { //check cccam-cardlist:
								cur_card_check += cc->card_added_count;
								cur_card_check += cc->card_removed_count;
								card_count += ll_count(cc->cards);
						}
						cur_check = crc32(cur_check, (uint8_t*)&rdr->tcp_connected, sizeof(rdr->tcp_connected));
						cur_check = crc32(cur_check, (uint8_t*)&rdr->card_status, sizeof(rdr->card_status));
						
						//Check hexserial/UA changes only on lokal readers:
						if (!(rdr->typ & R_IS_NETWORK)) {
							cur_check = crc32(cur_check, (uint8_t*)&rdr->hexserial, 8); //check hexserial
							cur_check = crc32(cur_check, (uint8_t*)&rdr->prid, rdr->nprov * sizeof(rdr->prid[0])); //check providers
							cur_check = crc32(cur_check, (uint8_t*)&rdr->sa, rdr->nprov * sizeof(rdr->sa[0])); //check provider-SA
						}
						
						cur_check = crc32(cur_check, (uint8_t*)&rdr->ftab, sizeof(FTAB)); //check reader 
						cur_check = crc32(cur_check, (uint8_t*)&rdr->ctab, sizeof(CAIDTAB)); //check caidtab
						cur_check = crc32(cur_check, (uint8_t*)&rdr->fchid, sizeof(FTAB)); //check chids
						cur_check = crc32(cur_check, (uint8_t*)&rdr->sidtabok, sizeof(rdr->sidtabok)); //check assigned ok services
						cur_check = crc32(cur_check, (uint8_t*)&rdr->sidtabno, sizeof(rdr->sidtabno)); //check assigned no services
				}
				
				//check defined services:
				struct s_sidtab *ptr;
		        for (ptr=cfg.sidtab; ptr; ptr=ptr->next) {
		        		cur_check = crc32(cur_check, (uint8_t*)ptr, sizeof(struct s_sidtab));
				}
				
				//update cardlist if reader config has changed, also set interval to 1s / 30times
				if (cur_check != last_check) {
						i = DEFAULT_INTERVAL;
						cs_debug_mask(D_TRACE, "share-update [1] %lu %lu", cur_check, last_check); 
						refresh_shares();
						last_check = cur_check;
						last_card_check = cur_card_check;
				}
				//update cardlist if cccam cards has changed:
				else if (cur_card_check != last_card_check) {
						cs_debug_mask(D_TRACE, "share-update [2] %lu %lu", cur_card_check, last_card_check); 
						refresh_shares();
						last_card_check = cur_card_check;
				}
		}
}

int32_t compare_cards_by_hop(struct cc_card **pcard1, struct cc_card **pcard2)
{
	struct cc_card *card1 = (*pcard1), *card2 = (*pcard2);
	
	int32_t res = card1->hop - card2->hop;
	if (res) return res;
	res = card1->caid - card2->caid;
	if (res) return res;
	res = card1->reshare - card2->reshare;
	if (res) return res;
	res = card1->id - card2->id;
	return res; 
}

int32_t compare_cards_by_hop_r(struct cc_card **pcard1, struct cc_card **pcard2)
{
	return -compare_cards_by_hop(pcard1, pcard2);
}

void sort_cards_by_hop(LLIST *cards, int32_t reverse)
{
	if (reverse)
		ll_sort(cards, compare_cards_by_hop_r);
	else
		ll_sort(cards, compare_cards_by_hop);
}

void init_share() {

		reported_carddatas = ll_create();
		cs_lock_create(&cc_shares_lock, 10, "cc_shares_lock");

		share_updater_thread = 0;
		pthread_t temp;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
#ifndef TUXBOX
        pthread_attr_setstacksize(&attr, PTHREAD_STACK_SIZE);
#endif
        if (pthread_create(&temp, &attr, (void*)&share_updater, NULL))
        		cs_log("ERROR: can't create share updater thread!");
		else {
        		cs_debug_mask(D_TRACE, "share updater thread started");
        		pthread_detach(temp);
        		share_updater_thread = temp;
        }
        pthread_attr_destroy(&attr);
}            

void done_share() {
		if (share_updater_thread) {
				pthread_cancel(share_updater_thread);
				share_updater_thread = 0;
				
				cc_free_reported_carddata(reported_carddatas, NULL, 0);
		}
}
#endif
