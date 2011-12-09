#include "globals.h"

//CMD00 - ECM (request)
//CMD01 - ECM (response)
//CMD02 - EMM (in clientmode - set EMM, in server mode - EMM data) - obsolete
//CMD03 - ECM (cascading request)
//CMD04 - ECM (cascading response)
//CMD05 - EMM (emm request) send cardata/cardinfo to client
//CMD06 - EMM (incomming EMM in server mode)
//CMD19 - EMM (incomming EMM in server mode) only seen with caid 0x1830
//CMD08 - Stop sending requests to the server for current srvid,prvid,caid
//CMD44 - MPCS/OScam internal error notification

#define REQ_SIZE	584		// 512 + 20 + 0x34

static int32_t camd35_send(uchar *buf)
{
	int32_t l, buflen;
	unsigned char rbuf[REQ_SIZE+15+4], *sbuf = rbuf + 4;
	struct s_client *cl = cur_client();

	if (!cl->udp_fd) return(-1);

	//Fix ECM len > 255
	buflen = ((buf[0] == 0)? (((buf[21]&0x0f)<< 8) | buf[22])+3 : buf[1]);
	l = 20 + (((buf[0] == 3) || (buf[0] == 4)) ? 0x34 : 0) + buflen;
	memcpy(rbuf, cl->ucrc, 4);
	memcpy(sbuf, buf, l);
	memset(sbuf + l, 0xff, 15);	// set unused space to 0xff for newer camd3's
	i2b_buf(4, crc32(0L, sbuf+20, buflen), sbuf + 4);
	l = boundary(4, l);
	cs_ddump_mask(D_CLIENT, sbuf, l, "send %d bytes to %s", l, remote_txt());
	aes_encrypt(sbuf, l);

        int32_t status;
	if (cl->is_udp) {
	   status = sendto(cl->udp_fd, rbuf, l+4, 0,
				           (struct sockaddr *)&cl->udp_sa,
				            sizeof(cl->udp_sa));
           if (status == -1) cl->udp_sa.sin_addr.s_addr = 0;
        }
	else {
	   status = send(cl->udp_fd, rbuf, l + 4, 0);
	   if (status == -1) network_tcp_connection_close(cl, cl->pfd);
        }
	return status;		
}

static int32_t camd35_auth_client(uchar *ucrc)
{
  int32_t rc=1;
  uint32_t crc;
  struct s_auth *account;
  struct s_client *cl = cur_client();
  unsigned char md5tmp[MD5_DIGEST_LENGTH];

  if (cl->upwd[0])
    return(memcmp(cl->ucrc, ucrc, 4) ? 1 : 0);
  cl->crypted=1;
  crc=(((ucrc[0]<<24) | (ucrc[1]<<16) | (ucrc[2]<<8) | ucrc[3]) & 0xffffffffL);
  for (account=cfg.account; (account) && (!cl->upwd[0]); account=account->next)
    if (crc==crc32(0L, MD5((unsigned char *)account->usr, strlen(account->usr), md5tmp), MD5_DIGEST_LENGTH))
    {
      memcpy(cl->ucrc, ucrc, 4);
      cs_strncpy((char *)cl->upwd, account->pwd, sizeof(cl->upwd));
      aes_set_key((char *) MD5(cl->upwd, strlen((char *)cl->upwd), md5tmp));
      rc=cs_auth_client(cl, account, NULL);
    }
  return(rc);
}

static int32_t camd35_recv(struct s_client *client, uchar *buf, int32_t l)
{
	int32_t rc, s, rs, n=0, buflen=0, len=0;
	for (rc=rs=s=0; !rc; s++) {
		switch(s) {
			case 0:
				if (!client->udp_fd) return(-9);
				if (client->is_udp && client->typ == 'c') {
					rs=recv_from_udpipe(buf);
				} else {
					//read minimum packet size (4 byte ucrc + 32 byte data) to detect packet size (tcp only)
					rs = recv(client->udp_fd, buf, client->is_udp ? l : 36, 0);
				}
				if (rs < 24) rc = -1;
				break;
			case 1:
				switch (camd35_auth_client(buf)) {
					case  0:        break;	// ok
					case  1: rc=-2; break;	// unknown user
					default: rc=-9; break;	// error's from cs_auth()
				}
				memmove(buf, buf+4, rs-=4);
				break;
			case 2:
				aes_decrypt(buf, rs);
				if (rs!=boundary(4, rs))
					cs_debug_mask(D_CLIENT, "WARNING: packet size has wrong decryption boundary");

				n=(buf[0]==3) ? 0x34 : 0;

				//Fix for ECM request size > 255 (use ecm length field)
				if(buf[0]==0)
					buflen = (((buf[21]&0x0f)<< 8) | buf[22])+3;
				else
					buflen = buf[1];

				n = boundary(4, n+20+buflen);
				if (!(client->is_udp && client->typ == 'c') && (rs < n) && ((n-32) > 0)) {
					len = recv(client->udp_fd, buf+32, n-32, 0); // read the rest of the packet
					if (len>0) {
						rs+=len;
						aes_decrypt(buf+32, len);
					}
				}

				cs_ddump_mask(D_CLIENT, buf, rs, "received %d bytes from %s", rs, remote_txt());

				if (n<rs)
					cs_debug_mask(D_CLIENT, "ignoring %d bytes of garbage", rs-n);
				else
					if (n>rs) rc=-3;
				break;
			case 3:
				if (crc32(0L, buf+20, buflen)!=b2i(4, buf+4)) rc=-4;
				if (!rc) rc=n;
				break;
		}
	}

	if ((rs>0) && ((rc==-1)||(rc==-2))) {
		cs_ddump_mask(D_CLIENT, buf, rs, "received %d bytes from %s (native)", rs, remote_txt);
	}
	client->last=time((time_t *) 0);

	switch(rc) {
		case -1:	cs_log("packet to small (%d bytes)", rs);		break;
		case -2:	cs_auth_client(client, 0, "unknown user");	break;
		case -3:	cs_log("incomplete request !");			break;
		case -4:	cs_log("checksum error (wrong password ?)");	break;
	}

	return(rc);
}

/*
 *	server functions
 */

static void camd35_request_emm(ECM_REQUEST *er)
{
	int32_t i;
	time_t now;
	uchar mbuf[1024];
	struct s_client *cl = cur_client();
	struct s_reader *aureader = NULL, *rdr = NULL;

	if (ll_contains(cl->aureader_list, er->selected_reader) && !er->selected_reader->audisabled)
		aureader = er->selected_reader;

	if (!aureader) {
		LL_ITER itr = ll_iter_create(cl->aureader_list);
		while ((rdr = ll_iter_next(&itr))) {
			if (emm_reader_match(rdr, er->caid, er->prid)) {
				aureader=rdr;
				break;
			}
		}
	}

	if (!aureader)
		return;  // TODO

	time(&now);
	if (!memcmp(cl->lastserial, aureader->hexserial, 8))
		if (abs(now-cl->last) < 180) return;

	memcpy(cl->lastserial, aureader->hexserial, 8);
	cl->last = now;

	if (aureader->caid)
	{
		cl->disable_counter = 0;
		log_emm_request(aureader);
	}
	else
		if (cl->disable_counter > 2)
			return;
		else
			cl->disable_counter++;

	memset(mbuf, 0, sizeof(mbuf));
	mbuf[2] = mbuf[3] = 0xff;			// must not be zero
	i2b_buf(2, er->srvid, mbuf + 8);

	//override request provid with auprovid if set in CMD05
	if(aureader->auprovid) {
		if(aureader->auprovid != er->prid)
			i2b_buf(4, aureader->auprovid, mbuf + 12);
		else
			i2b_buf(4, er->prid, mbuf + 12);
	} else {
		i2b_buf(4, er->prid, mbuf + 12);
	}

	i2b_buf(2, er->pid, mbuf + 16);
	mbuf[0] = 5;
	mbuf[1] = 111;
	if (aureader->caid)
	{
		mbuf[39] = 1;							// no. caids
		mbuf[20] = aureader->caid>>8;		// caid's (max 8)
		mbuf[21] = aureader->caid&0xff;
		memcpy(mbuf + 40, aureader->hexserial, 6);	// serial now 6 bytes
		mbuf[47] = aureader->nprov;
		for (i = 0; i < aureader->nprov; i++)
		{
			if (((aureader->caid >= 0x1700) && (aureader->caid <= 0x1799))  || // Betacrypt
					((aureader->caid >= 0x0600) && (aureader->caid <= 0x0699)))    // Irdeto (don't know if this is correct, cause I don't own a IRDETO-Card)
			{
				mbuf[48 + (i*5)] = aureader->prid[i][0];
				memcpy(&mbuf[50 + (i*5)], &aureader->prid[i][1], 3);
			}
			else
			{
				mbuf[48 + (i * 5)] = aureader->prid[i][2];
				mbuf[49 + (i * 5)] =aureader->prid[i][3];
				memcpy(&mbuf[50 + (i * 5)], &aureader->sa[i][0],4); // for conax we need at least 4 Bytes
			}
		}
		//we think client/server protocols should deliver all information, and only readers should discard EMM
		mbuf[128] = (aureader->blockemm & EMM_GLOBAL) ? 0: 1;
		mbuf[129] = (aureader->blockemm & EMM_SHARED) ? 0: 1;
		mbuf[130] = (aureader->blockemm & EMM_UNIQUE) ? 0: 1;
		//mbuf[131] = aureader->card_system; //Cardsystem for Oscam client
	}
	else		// disable emm
		mbuf[20] = mbuf[39] = mbuf[40] = mbuf[47] = mbuf[49] = 1;

	memcpy(mbuf + 10, mbuf + 20, 2);
	camd35_send(mbuf);		// send with data-len 111 for camd3 > 3.890
	mbuf[1]++;
	camd35_send(mbuf);		// send with data-len 112 for camd3 < 3.890
}

static void camd35_send_dcw(struct s_client *client, ECM_REQUEST *er)
{
	uchar *buf;
	buf = client->req + (er->cpti * REQ_SIZE);	// get orig request

	if (((er->rcEx > 0) || (er->rc == E_INVALID)) && !client->c35_suppresscmd08)
	{
		buf[0] = 0x08;
		buf[1] = 2;
		memset(buf + 20, 0, buf[1]);
		buf[22] = er->rc; //put rc in byte 22 - hopefully don't break legacy camd3
	}
	else if (er->rc == E_STOPPED)
	{
		buf[0] = 0x08;
		buf[1] = 2;
		buf[20] = 0;
		/*
		 * the second Databyte should be forseen for a sleeptime in minutes
		 * whoever knows the camd3 protocol related to CMD08 - please help!
		 * on tests this don't work with native camd3
		 */
		buf[21] = client->c35_sleepsend;
		cs_log("%s stop request send", client->account->usr);
	}
	else
	{
		// Send CW
		if ((er->rc < E_NOTFOUND) || (er->rc == E_FAKE))
		{
			if (buf[0]==3)
				memmove(buf + 20 + 16, buf + 20 + buf[1], 0x34);
			buf[0]++;
			buf[1] = 16;
			memcpy(buf+20, er->cw, buf[1]);
		}
		else
		{
			// Send old CMD44 to prevent cascading problems with older mpcs/oscam versions
			buf[0] = 0x44;
			buf[1] = 0;
		}
	}
	camd35_send(buf);
	camd35_request_emm(er);
}

static void camd35_process_ecm(uchar *buf)
{
	ECM_REQUEST *er;
	if (!(er = get_ecmtask()))
		return;
//	er->l = buf[1];
	//fix ECM LEN issue
	er->l =(((buf[21]&0x0f)<< 8) | buf[22])+3;
	memcpy(cur_client()->req + (er->cpti*REQ_SIZE), buf, 0x34 + 20 + er->l);	// save request
	er->srvid = b2i(2, buf+ 8);
	er->caid = b2i(2, buf+10);
	er->prid = b2i(4, buf+12);
	er->pid  = b2i(2, buf+16);
	memcpy(er->ecm, buf + 20, er->l);
	get_cw(cur_client(), er);
}

static void camd35_process_emm(uchar *buf)
{
	EMM_PACKET epg;
	memset(&epg, 0, sizeof(epg));
	epg.l = buf[1];
	memcpy(epg.caid, buf + 10, 2);
	memcpy(epg.provid, buf + 12 , 4);
	memcpy(epg.emm, buf + 20, epg.l);
	do_emm(cur_client(), &epg);
}

static void * camd35_server(void *cli)
{
  int32_t n;
  uchar mbuf[1024];

	struct s_client * client = (struct s_client *) cli;
  client->thread=pthread_self();
  pthread_setspecific(getclient, cli);

	cs_malloc(&client->req,CS_MAXPENDING*REQ_SIZE, 1);

  client->is_udp = (ph[client->ctyp].type == MOD_CONN_UDP);

  while ((n=process_input(mbuf, sizeof(mbuf), cfg.cmaxidle))>0)
  {
    switch(mbuf[0])
    {
      case  0:	// ECM
      case  3:	// ECM (cascading)
        camd35_process_ecm(mbuf);
        break;
      case  6:	// EMM
      case 19:  // EMM
        camd35_process_emm(mbuf);
        break;
      default:
        cs_log("unknown camd35 command! (%d)", mbuf[0]);
    }
  }

  NULLFREE(client->req);

  cs_disconnect_client(client);
  return NULL; //to prevent compiler message
}

/*
 *	client functions
 */

static void casc_set_account()
{
	unsigned char md5tmp[MD5_DIGEST_LENGTH];
  struct s_client *cl = cur_client();
  cs_strncpy((char *)cl->upwd, cl->reader->r_pwd, sizeof(cl->upwd));
  i2b_buf(4, crc32(0L, MD5((unsigned char *)cl->reader->r_usr, strlen(cl->reader->r_usr), md5tmp), 16), cl->ucrc);
  aes_set_key((char *)MD5(cl->upwd, strlen((char *)cl->upwd), md5tmp));
  cl->crypted=1;
}

int32_t camd35_client_init(struct s_client *client)
{
  struct sockaddr_in loc_sa;
  char ptxt[16];

  client->pfd=0;
  if (client->reader->r_port<=0)
  {
    cs_log("invalid port %d for server %s", client->reader->r_port, client->reader->device);
    return(1);
  }
  client->is_udp=(client->reader->typ==R_CAMD35);

  client->ip=0;
  memset((char *)&loc_sa,0,sizeof(loc_sa));
  loc_sa.sin_family = AF_INET;
#ifdef LALL
  if (cfg.serverip[0])
    loc_sa.sin_addr.s_addr = inet_addr(cfg.serverip);
  else
#endif
    loc_sa.sin_addr.s_addr = INADDR_ANY;
  loc_sa.sin_port = htons(client->reader->l_port);

  if ((client->udp_fd=socket(PF_INET, client->is_udp ? SOCK_DGRAM : SOCK_STREAM, client->is_udp ? IPPROTO_UDP : IPPROTO_TCP))<0)
  {
    cs_log("Socket creation failed (errno=%d %s)", errno, strerror(errno));
    cs_exit(1);
  }

#ifdef SO_PRIORITY
  if (cfg.netprio)
    setsockopt(client->udp_fd, SOL_SOCKET, SO_PRIORITY, (void *)&cfg.netprio, sizeof(uintptr_t));
#endif

  if (client->reader->l_port>0)
  {
    if (bind(client->udp_fd, (struct sockaddr *)&loc_sa, sizeof (loc_sa))<0)
    {
      cs_log("bind failed (errno=%d %s)", errno, strerror(errno));
      close(client->udp_fd);
      return(1);
    }
    snprintf(ptxt, sizeof(ptxt), ", port=%d", client->reader->l_port);
  }
  else
    ptxt[0]='\0';

  casc_set_account();
  memset((char *)&client->udp_sa, 0, sizeof(client->udp_sa));
  client->udp_sa.sin_family=AF_INET;
  client->udp_sa.sin_port=htons((uint16_t)client->reader->r_port);

  cs_log("proxy %s:%d (fd=%d%s)",
         client->reader->device, client->reader->r_port,
         client->udp_fd, ptxt);

  if (client->is_udp) {
  	client->pfd=client->udp_fd;
  }

  return(0);
}

int32_t camd35_client_init_log()
{
  struct sockaddr_in loc_sa;
  struct s_client *cl = cur_client();

  if (cl->reader->log_port<=0)
  {
    cs_log("invalid port %d for camd3-loghost", cl->reader->log_port);
    return(1);
  }

  memset((char *)&loc_sa,0,sizeof(loc_sa));
  loc_sa.sin_family = AF_INET;
  loc_sa.sin_addr.s_addr = INADDR_ANY;
  loc_sa.sin_port = htons(cl->reader->log_port);

  if ((logfd=socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP))<0)
  {
    cs_log("Socket creation failed (errno=%d %s)", errno, strerror(errno));
    return(1);
  }

  if (bind(logfd, (struct sockaddr *)&loc_sa, sizeof(loc_sa))<0)
  {
    cs_log("bind failed (errno=%d %s)", errno, strerror(errno));
    close(logfd);
    return(1);
  }

  cs_log("camd3 loghost initialized (fd=%d, port=%d)",
         logfd, cl->reader->log_port);

  return(0);
}

static int32_t tcp_connect()
{
  struct s_client *cl = cur_client();
  if (!cl->reader->tcp_connected)
  {
    int32_t handle=0;
    handle = network_tcp_connection_open();
    if (handle<0) return(0);

    cl->reader->tcp_connected = 1;
    cl->reader->card_status = CARD_INSERTED;
    cl->reader->last_s = cl->reader->last_g = time((time_t *)0);
    cl->pfd = cl->udp_fd = handle;
  }
  if (!cl->udp_fd) return(0);
  return(1);
}

static int32_t camd35_send_ecm(struct s_client *client, ECM_REQUEST *er, uchar *buf)
{
	static const char *typtext[]={"ok", "invalid", "sleeping"};

	if (client->stopped) {
		if (er->srvid == client->lastsrvid && er->caid == client->lastcaid && er->pid == client->lastpid){
			cs_log("%s is stopped - requested by server (%s)",
					client->reader->label, typtext[client->stopped]);
			return(-1);
		}
		else {
			client->stopped = 0;
		}
	}
	
	client->lastsrvid = er->srvid;
	client->lastcaid = er->caid;
	client->lastpid = er->pid;

	if (client->is_udp) {
	   if (!client->udp_sa.sin_addr.s_addr || client->reader->last_s-client->reader->last_g > client->reader->tcp_rto)
	      if (!hostResolve(client->reader)) return -1;
	}
        else {
  	   if (!tcp_connect()) return -1;
        }
	
	client->reader->card_status = CARD_INSERTED; //for udp
	
	memset(buf, 0, 20);
	memset(buf + 20, 0xff, er->l+15);
	buf[1]=er->l;
	i2b_buf(2, er->srvid, buf + 8);
	i2b_buf(2, er->caid, buf + 10);
	i2b_buf(4, er->prid, buf + 12);
	//  i2b_buf(2, er->pid,, buf + 16);
	//  memcpy(buf+16, &er->idx , 2);
	i2b_buf(2, er->idx, buf + 16);
	buf[18] = 0xff;
	buf[19] = 0xff;
	memcpy(buf + 20, er->ecm, er->l);
	return((camd35_send(buf) < 1) ? (-1) : 0);
}

static int32_t camd35_send_emm(EMM_PACKET *ep)
{
	uchar buf[512];
  struct s_client *cl = cur_client();
	
        if (cl->is_udp) {
           if (!cl->udp_sa.sin_addr.s_addr || cl->reader->last_s-cl->reader->last_g > cl->reader->tcp_rto)
              if (!hostResolve(cl->reader)) return -1;
        }
        else {
           if (!tcp_connect()) return -1;
        }
	
	memset(buf, 0, 20);
	memset(buf+20, 0xff, ep->l+15);

	buf[0]=0x06;
	buf[1]=ep->l;
	memcpy(buf+10, ep->caid, 2);
	memcpy(buf+12, ep->provid, 4);
	memcpy(buf+20, ep->emm, ep->l);

	return((camd35_send(buf)<1) ? 0 : 1);
}

static int32_t camd35_recv_chk(struct s_client *client, uchar *dcw, int32_t *rc, uchar *buf, int32_t UNUSED(n))
{
	uint16_t idx;
	static const char *typtext[]={"ok", "invalid", "sleeping"};
  struct s_reader *rdr = client->reader;

	// reading CMD05 Emm request and set serial
	if (buf[0] == 0x05 && buf[1] == 111) {

		//cs_log("CMD05: %s", cs_hexdump(1, buf, buf[1], tmp, sizeof(tmp)));
		rdr->nprov = 0; //reset if number changes on reader change
		rdr->nprov = buf[47];
		rdr->caid = b2i(2, buf + 20);
		rdr->auprovid = b2i(4, buf + 12);

		int32_t i;
		for (i=0; i<rdr->nprov; i++) {
			if (((rdr->caid >= 0x1700) && (rdr->caid <= 0x1799))  ||	// Betacrypt
					((rdr->caid >= 0x0600) && (rdr->caid <= 0x0699)))	// Irdeto (don't know if this is correct, cause I don't own a IRDETO-Card)
			{
				rdr->prid[i][0] = buf[48 + (i*5)];
				memcpy(&rdr->prid[i][1], &buf[50 + (i * 5)], 3);
			} else {
				rdr->prid[i][2] = buf[48 + (i * 5)];
				rdr->prid[i][3] = buf[49+ (i * 5)];
				memcpy(&rdr->sa[i][0], &buf[50 + (i * 5)], 4);
			}
		}

		memcpy(rdr->hexserial, buf + 40, 6);
		rdr->hexserial[6] = 0;
		rdr->hexserial[7] = 0;

		rdr->blockemm = 0;
		rdr->blockemm |= (buf[128]==1) ? 0 : EMM_GLOBAL;
		rdr->blockemm |= (buf[129]==1) ? 0 : EMM_SHARED;
		rdr->blockemm |= (buf[130]==1) ? 0 : EMM_UNIQUE;
		cs_log("%s CMD05 AU request for caid: %04X auprovid: %06lX",
				rdr->label,
				rdr->caid,
				rdr->auprovid);
	}

	if (buf[0] == 0x08 && ((rdr->ph.type == MOD_CONN_TCP && !cfg.c35_tcp_suppresscmd08) || (rdr->ph.type == MOD_CONN_UDP && !cfg.c35_udp_suppresscmd08))) {
		if(buf[21] == 0xFF) {
			client->stopped = 2; // server says sleep
			rdr->card_status = NO_CARD;
		} else {
#ifdef WITH_LB
		        if (!cfg.lb_mode) {
#endif
			        client->stopped = 1; // server says invalid
                                rdr->card_status = CARD_FAILURE;
#ifdef WITH_LB
                        }
#endif
		}

		cs_log("%s CMD08 (%02X - %d) stop request by server (%s)",
				rdr->label, buf[21], buf[21], typtext[client->stopped]);
	}

	// CMD44: old reject command introduced in mpcs
	// keeping this for backward compatibility
	if ((buf[0] != 1) && (buf[0] != 0x44) && (buf[0] != 0x08))
		return(-1);

	idx = b2i(2, buf+16);

	*rc = ((buf[0] != 0x44) && (buf[0] != 0x08));

	memcpy(dcw, buf+20, 16);
	return(idx);
}

static int32_t camd35_recv_log(uint16_t *caid, uint32_t *provid, uint16_t *srvid)
{
  int32_t i;
  uchar buf[512], *ptr, *ptr2;
  char *saveptr1 = NULL;
  uint16_t idx=0;
  if (!logfd) return(-1);
  if ((i=recv(logfd, buf, sizeof(buf), 0))<=0) return(-1);
  buf[i]=0;

  if (!(ptr=(uchar *)strstr((char *)buf, " -> "))) return(-1);
  ptr+=4;
  if (strstr((char *)ptr, " decoded ")) return(-1);	// skip "found"s
  if (!(ptr2=(uchar *)strchr((char *)ptr, ' '))) return(-1);	// corrupt
  *ptr2=0;

  for (i=0, ptr2=(uchar *)strtok_r((char *)ptr, ":", &saveptr1); ptr2; i++, ptr2=(uchar *)strtok_r(NULL, ":", &saveptr1))
  {
    trim((char *)ptr2);
    switch(i)
    {
      case 0: *caid  =cs_atoi((char *)ptr2, strlen((char *)ptr2)>>1, 0); break;
      case 1: *provid=cs_atoi((char *)ptr2, strlen((char *)ptr2)>>1, 0); break;
      case 2: *srvid =cs_atoi((char *)ptr2, strlen((char *)ptr2)>>1, 0); break;
      case 3: idx    =cs_atoi((char *)ptr2, strlen((char *)ptr2)>>1, 0); break;
    }
    if (errno) return(-1);
  }
  return(idx&0x1FFF);
}

/*
 *	module definitions
 */

void module_camd35(struct s_module *ph)
{
  static PTAB ptab; //since there is always only 1 camd35 server running, this is threadsafe
  ptab.ports[0].s_port = cfg.c35_port;
  ph->ptab = &ptab;
  ph->ptab->nports = 1;

  cs_strncpy(ph->desc, "camd35", sizeof(ph->desc));
  ph->type=MOD_CONN_UDP;
  ph->listenertype = LIS_CAMD35UDP;
  ph->multi=1;
  ph->watchdog=1;
  ph->s_ip=cfg.c35_srvip;
  ph->s_handler=camd35_server;
  ph->recv=camd35_recv;
  ph->send_dcw=camd35_send_dcw;
  ph->c_multi=1;
  ph->c_init=camd35_client_init;
  ph->c_recv_chk=camd35_recv_chk;
  ph->c_send_ecm=camd35_send_ecm;
  ph->c_send_emm=camd35_send_emm;
  ph->c_init_log=camd35_client_init_log;
  ph->c_recv_log=camd35_recv_log;
  ph->num=R_CAMD35;
}

void module_camd35_tcp(struct s_module *ph)
{
  cs_strncpy(ph->desc, "cs378x", sizeof(ph->desc));
  ph->type=MOD_CONN_TCP;
  ph->listenertype = LIS_CAMD35TCP;
  ph->multi=1;
  ph->watchdog=1;
  ph->ptab=&cfg.c35_tcp_ptab;
  if (ph->ptab->nports==0)
    ph->ptab->nports=1; // show disabled in log
  ph->s_ip=cfg.c35_tcp_srvip;
  ph->s_handler=camd35_server;
  ph->recv=camd35_recv;
  ph->send_dcw=camd35_send_dcw;
  ph->c_multi=1;
  ph->c_init=camd35_client_init;
  ph->c_recv_chk=camd35_recv_chk;
  ph->c_send_ecm=camd35_send_ecm;
  ph->c_send_emm=camd35_send_emm;
  ph->c_init_log=camd35_client_init_log;
  ph->c_recv_log=camd35_recv_log;
  ph->num=R_CS378X;
}
