#include "globals.h"

#define CS_NANO_CLASS 0xE2

static int32_t find_nano(uchar *ecm, int32_t l, uchar nano, int32_t s)
{
  uchar *snano;

  if( s >= l ) return 0;
  if( !s ) s=(ecm[4]==0xD2) ? 12 : 9;	// tpsflag -> offset+3
  snano = ecm + s;

  while( (*snano!=nano) && (s<l) )
  {
    if( *snano == 0xEA ) return 0;
    snano++;
    s++;
  }

  return (s<l)?++s:0;
}

static int32_t chk_class(ECM_REQUEST *er, CLASSTAB *clstab, const char *D_USE(type), const char *D_USE(name))
{
  int32_t i, j, an, cl_n, l;
  uchar ecm_class;

  if( er->caid!=0x0500 ) return 1;
  if( !clstab->bn && !clstab->an ) return 1;

  j=an=cl_n=l=0;
  while( (j=find_nano(er->ecm, er->l, CS_NANO_CLASS, j)) > 0 )
  {
    l = er->ecm[j];
    ecm_class = er->ecm[j+l];
    cs_debug_mask(D_CLIENT, "ecm class=%02X", ecm_class);
    for( i=0; i<clstab->bn; i++ )  // search in blocked
      if( ecm_class==clstab->bclass[i] ) 
      {
        cs_debug_mask(D_CLIENT, "class %02X rejected by %s '%s' !%02X filter", 
                 ecm_class, type, name, ecm_class);
        return 0;
      }

    cl_n++;
    for( i=0; i<clstab->an; i++ )  // search in allowed
      if( ecm_class==clstab->aclass[i] ) 
      {
        an++;
        break;
      }
    j+=l;
  }

  if( cl_n && clstab->an )
  {
    if( an ) 
      cs_debug_mask(D_CLIENT, "ECM classes allowed by %s '%s' filter", type, name);
    else {
      cs_debug_mask(D_CLIENT, "ECM classes don't match %s '%s' filter, rejecting", type, name);
      return 0;
    }
  }

  return 1;
}

int32_t chk_srvid_match(ECM_REQUEST *er, SIDTAB *sidtab)
{
  int32_t i, rc=0;

  if (!sidtab->num_caid)
    rc|=1;
  else
    for (i=0; (i<sidtab->num_caid) && (!(rc&1)); i++)
      if (er->caid==sidtab->caid[i]) rc|=1;

  if (!sidtab->num_provid)
    rc|=2;
  else
    for (i=0; (i<sidtab->num_provid) && (!(rc&2)); i++)
      if (er->prid==sidtab->provid[i]) rc|=2;

  if (!sidtab->num_srvid)
    rc|=4;
  else
    for (i=0; (i<sidtab->num_srvid) && (!(rc&4)); i++)
      if (er->srvid==sidtab->srvid[i]) rc|=4;

  return(rc==7);
}

int32_t chk_srvid(struct s_client *cl, ECM_REQUEST *er)
{
  int32_t nr, rc=0;
  SIDTAB *sidtab;

  if (!cl->sidtabok)
  {
    if (!cl->sidtabno) return(1);
    rc=1;
  }
  for (nr=0, sidtab=cfg.sidtab; sidtab; sidtab=sidtab->next, nr++)
    if (sidtab->num_caid | sidtab->num_provid | sidtab->num_srvid)
    {
      if ((cl->sidtabno&((SIDTABBITS)1<<nr)) &&
          (chk_srvid_match(er, sidtab)))
        return(0);
      if ((cl->sidtabok&((SIDTABBITS)1<<nr)) &&
          (chk_srvid_match(er, sidtab)))
        rc=1;
    }
  return(rc);
}

int32_t has_srvid(struct s_client *cl, ECM_REQUEST *er) {
  if (!cl->sidtabok)
    return 0;

  int32_t nr;
  SIDTAB *sidtab;
      
  for (nr=0, sidtab=cfg.sidtab; sidtab; sidtab=sidtab->next, nr++)
    if (sidtab->num_srvid)
    {
      if ((cl->sidtabok&((SIDTABBITS)1<<nr)) &&
          (chk_srvid_match(er, sidtab)))
        return 1;
    }
  return 0;
}


static int32_t chk_srvid_match_by_caid_prov(uint16_t caid, uint32_t provid, SIDTAB *sidtab)
{
  int32_t i, rc=0;

  if (!sidtab->num_caid)
    rc|=1;
  else
    for (i=0; (i<sidtab->num_caid) && (!(rc&1)); i++)
      if (caid==sidtab->caid[i]) rc|=1;

  if (!sidtab->num_provid)
    rc|=2;
  else
    for (i=0; (i<sidtab->num_provid) && (!(rc&2)); i++)
      if (provid==sidtab->provid[i]) rc|=2;

  return(rc==3);
}

int32_t chk_srvid_by_caid_prov(struct s_client *cl, uint16_t caid, uint32_t provid) {
  int32_t nr, rc=0;
  SIDTAB *sidtab;

  if (!cl->sidtabok)
  {
    if (!cl->sidtabno) return(1);
    rc=1;
  }
  for (nr=0, sidtab=cfg.sidtab; sidtab; sidtab=sidtab->next, nr++)
    if (sidtab->num_caid | sidtab->num_provid)
    {
      if ((cl->sidtabno&((SIDTABBITS)1<<nr)) && !sidtab->num_srvid &&
          (chk_srvid_match_by_caid_prov(caid, provid, sidtab)))
        return(0);
      if ((cl->sidtabok&((SIDTABBITS)1<<nr)) &&
          (chk_srvid_match_by_caid_prov(caid, provid, sidtab)))
        rc=1;
    }
  return(rc);

}

// server filter for newcamd
int32_t chk_sfilter(ECM_REQUEST *er, PTAB *ptab)
{
  int32_t i, j, pi, rc=1;
  uint16_t caid, scaid;
  uint32_t  prid, sprid;

  if (!ptab) return(1);
  struct s_client *cur_cl = cur_client();
  
  caid = er->caid;
  prid = er->prid;
  pi = cur_cl->port_idx;

  if (ptab->nports && ptab->ports[pi].ftab.nfilts)
  {
    for( rc=j=0; (!rc) && (j<ptab->ports[pi].ftab.nfilts); j++ )
    {
      scaid = ptab->ports[pi].ftab.filts[j].caid;
      if (caid==0||(caid!=0 && caid==scaid))
      {
        for( i=0; (!rc) && i<ptab->ports[pi].ftab.filts[j].nprids; i++ )
        {
          sprid=ptab->ports[pi].ftab.filts[j].prids[i];
          cs_debug_mask(D_CLIENT, "trying server filter %04X:%06X", scaid, sprid);
          if (prid==sprid)
          {
            rc=1;
            cs_debug_mask(D_CLIENT, "%04X:%06X allowed by server filter %04X:%06X",
                     caid, prid, scaid, sprid);
          }
        }
      }
    }
    if(!rc)
    {
      cs_debug_mask(D_CLIENT, "no match, %04X:%06X rejected by server filters", caid, prid);
      snprintf( er->msglog, MSGLOGSIZE, "no server match %04X:%06X",
        caid, (uint32_t) prid );

      if (!er->rcEx) er->rcEx=(E1_LSERVER<<4)|E2_IDENT;
      return(rc);
    }
  }
  return (rc);
}

static int32_t chk_chid(ECM_REQUEST *er, FTAB *fchid, char *D_USE(type), char *D_USE(name))
{
  int32_t rc=1, i, j;

  if( (er->caid & 0xFF00)!=0x600 ) return 1;
  if( !er->chid ) return 1;
  if( !fchid->nfilts ) return 1;

  for( i=rc=0; (!rc) && i<fchid->nfilts; i++ )
    if( er->caid == fchid->filts[i].caid )
      for( j=0; (!rc) && j<fchid->filts[i].nprids; j++ )
      {
        cs_debug_mask(D_CLIENT, "trying %s '%s' CHID filter %04X:%04X", 
                 type, name, fchid->filts[i].caid, fchid->filts[i].prids[j]);
        if( er->chid == fchid->filts[i].prids[j] )
        {
          cs_debug_mask(D_CLIENT, "%04X:%04X allowed by %s '%s' CHID filter %04X:%04X",
                   er->caid, er->chid, type, name, fchid->filts[i].caid, 
                   fchid->filts[i].prids[j]);
          rc=1;
        }
      }

  if( !rc )
  {
    cs_debug_mask(D_CLIENT, "no match, %04X:%04X rejected by %s '%s' CHID filter(s)", 
                      er->caid, er->chid, type, name);
  }
  return (rc);
}

int32_t chk_ufilters(ECM_REQUEST *er)
{
  int32_t i, j, rc;
  uint16_t ucaid;
  uint32_t  uprid;
  struct s_client *cur_cl = cur_client();
  
  rc=1;
  if( cur_cl->ftab.nfilts )
  {
    FTAB *f = &cur_cl->ftab;
    for( i=rc=0; (!rc) && (i<f->nfilts); i++ )
    {
      ucaid = f->filts[i].caid;
      if( er->caid==0 || ucaid==0 || (er->caid!=0 && er->caid==ucaid) )
      {
        for( j=rc=0; (!rc) && (j<f->filts[i].nprids); j++ )
        {
          uprid = f->filts[i].prids[j];
          cs_debug_mask(D_CLIENT, "trying user '%s' filter %04X:%06X",
                   cur_cl->account->usr, ucaid, uprid);
          if( er->prid == uprid )
          {
            rc=1;
            cs_debug_mask(D_CLIENT, "%04X:%06X allowed by user '%s' filter %04X:%06X",
                      er->caid, er->prid, cur_cl->account->usr, ucaid, uprid);
          }
        }
      }
    }
    if( !rc ) {
      cs_debug_mask(D_CLIENT, "no match, %04X:%06X rejected by user '%s' filters",
                er->caid, er->prid, cur_cl->account->usr);
        snprintf( er->msglog, MSGLOGSIZE, "no card support %04X:%06X",
                er->caid, (uint32_t) er->prid );

      if( !er->rcEx ) er->rcEx=(E1_USER<<4)|E2_IDENT;
      return (rc);
    }
  }

  if( !(rc=chk_class(er, &cur_cl->cltab, "user", cur_cl->account->usr)) ) {
    if( !er->rcEx ) er->rcEx=(E1_USER<<4)|E2_CLASS;
  }
  else if( !(rc=chk_chid(er, &cur_cl->fchid, "user", cur_cl->account->usr)) )
    if( !er->rcEx ) er->rcEx=(E1_USER<<4)|E2_CHID;

  if( rc ) er->rcEx=0;

  return (rc);
}

int32_t chk_rsfilter(struct s_reader * reader, ECM_REQUEST *er)
{
  int32_t i, rc=1;
  uint16_t caid;
  uint32_t prid;

  if( reader->ncd_disable_server_filt )
  { 
    cs_debug_mask(D_CLIENT, "%04X:%06X allowed - server filters disabled",
              er->caid, er->prid);
    return 1;
  }

  rc=prid=0;
  caid = reader->caid;
  if( caid==er->caid )
  {
    for( i=0; (!rc) && (i<reader->nprov); i++ )
    {
      prid = (uint32_t)((reader->prid[i][1]<<16) |
                     (reader->prid[i][2]<<8) |
                     (reader->prid[i][3]));
      cs_debug_mask(D_CLIENT, "trying server '%s' filter %04X:%06X", 
                reader->device, caid, prid);
      if( prid==er->prid )
      {
        rc=1;
        cs_debug_mask(D_CLIENT, "%04X:%06X allowed by server '%s' filter %04X:%06X",
                  er->caid, er->prid, reader->device, caid, prid);
      }
    }
  }
  if(!rc) {
    cs_debug_mask(D_CLIENT, "no match, %04X:%06X rejected by server '%s' filters",
            er->caid, er->prid, reader->device);
    if( !er->rcEx ) er->rcEx=(E1_SERVER<<4)|E2_IDENT;
    return 0;
  }

  return(rc);
}

static int32_t chk_rfilter(ECM_REQUEST *er, struct s_reader *rdr)
{
  int32_t i, j, rc=1;
  uint16_t caid=0;
  uint32_t prid=0;

  if( rdr->ftab.nfilts )
  { 
    for( rc=i=0; (!rc) && (i<rdr->ftab.nfilts); i++ )
    {
      caid = rdr->ftab.filts[i].caid;
      if( (caid!=0 && caid==er->caid) || caid==0 )
      { 
        for( j=0; (!rc) && (j<rdr->ftab.filts[i].nprids); j++)
        {
          prid = rdr->ftab.filts[i].prids[j];
          cs_debug_mask(D_CLIENT, "trying reader '%s' filter %04X:%06X",
                    rdr->label, caid, prid);
          if( prid==er->prid )
          {
            rc=1;
            cs_debug_mask(D_CLIENT, "%04X:%06X allowed by reader '%s' filter %04X:%06X",
                    er->caid, er->prid, rdr->label, caid, prid);
          }
        }
      }
    }
    if(!rc) {
      cs_debug_mask(D_CLIENT, "no match, %04X:%06X rejected by reader '%s' filters",
                er->caid, er->prid, rdr->label);
      return 0;
    }
  }

  return(rc);
}

int32_t chk_ctab(uint16_t caid, CAIDTAB *ctab) {
  if (!caid || !ctab->caid[0])
    return 1;
    
  int32_t i;
  for (i=0;i<CS_MAXCAIDTAB;i++)
  {
    if (!ctab->caid[i]) {
      return 0;
    }
    if ((caid & ctab->mask[i]) == ctab->caid[i])
      return 1;
  }
  return 0;
}

int32_t matching_reader(ECM_REQUEST *er, struct s_reader *rdr) {
  //simple checks first:
  if (!er || !rdr ||!rdr->client)
    return(0);

  //Checking connected & group valid:
  struct s_client *cur_cl = cur_client();
  
  if (!((rdr->fd) && (rdr->grp&cur_cl->grp)))
    return(0);

  //Schlocke reader-defined function, reader-self-check: 
  if (rdr->ph.c_available && !rdr->ph.c_available(rdr, AVAIL_CHECK_CONNECTED)) {
    //cs_debug_mask(D_TRACE, "reader unavailable %s", rdr->label);
    return 0;
  }

  //Checking caids:
  if (!chk_ctab(er->ocaid, &rdr->ctab) && !chk_ctab(er->caid, &rdr->ctab)) {
    //cs_debug_mask(D_TRACE, "caid %04X not found in caidlist reader %s", er->caid, rdr->label);
    return 0;
  }

  if ((!(rdr->typ & R_IS_NETWORK)) && ((rdr->caid >> 8) != ((er->caid >> 8) & 0xFF) && (rdr->caid >> 8) != ((er->ocaid >> 8) & 0xFF)))
    return 0;

  //Checking services:
  if (!chk_srvid(rdr->client, er)) {
    //cs_debug_mask(D_TRACE, "service %04X not matching  reader %s", er->srvid, rdr->label);
    return(0);
  }

  //Checking ident:
  if (!chk_rfilter(er, rdr)) {
    //cs_debug_mask(D_TRACE, "r-filter reader %s", rdr->label);
    return(0);
  }

  //Check ECM nanos:
  if (!chk_class(er, &rdr->cltab, "reader", rdr->label)) {
    //cs_debug_mask(D_TRACE, "class filter reader %s", rdr->label);    
    return(0);
  }

  //Checking chid:
  if (!chk_chid(er, &rdr->fchid, "reader", rdr->label)) {
    //cs_debug_mask(D_TRACE, "chid filter reader %s", rdr->label);    
    return(0);
  }
  
  //Checking ecmlength:
  if (rdr->ecmWhitelist){
  	struct s_ecmWhitelist *tmp;
  	struct s_ecmWhitelistIdent *tmpIdent;
  	struct s_ecmWhitelistLen *tmpLen;
  	int8_t ok = 0, foundident = 0;
  	for(tmp = rdr->ecmWhitelist; tmp; tmp = tmp->next){
  		if(tmp->caid == 0 || tmp->caid == er->caid){
  			for(tmpIdent = tmp->idents; tmpIdent; tmpIdent = tmpIdent->next){
  				if(tmpIdent->ident == 0 || tmpIdent->ident == er->prid){
  					foundident = 1;
			  		for(tmpLen = tmpIdent->lengths; tmpLen; tmpLen = tmpLen->next){
			  			if(tmpLen->len == er->l){
				  			ok = 1;
				  			break;
				  		}
			  		}
			  	}
			  }
	  	}  		
  	}
  	if(foundident == 1 && ok == 0){
  		cs_debug_mask(D_READER, "ECM is not in ecmwhitelist of reader %s.",rdr->label);
  		return(0);
  	}
  }
 
  //All checks done, reader is matching!
  return(1);
}

int32_t emm_reader_match(struct s_reader *reader, uint16_t caid, uint32_t provid) {
	int32_t i;

	if (reader->caid != caid || reader->audisabled) {
		return 0;
	}

	if (!hexserialset(reader)) {
		cs_debug_mask(D_EMM, "emm reader %s has no serial set", reader->label);
		return 0;
	}

	if (!provid) {
		cs_debug_mask(D_EMM, "emm for reader %s (%04X) has no provider", reader->label, caid);
		return 1;
	}

	if (reader->auprovid && reader->auprovid == provid) {
		cs_debug_mask(D_EMM, "emm provider match reader %s auprovid %06X", reader->label, reader->auprovid);
		return 1;
	}

	if (!reader->nprov) {
		cs_debug_mask(D_EMM, "emm reader %s has no provider set", reader->label);
		return 1;
	}

	for (i=0; i<reader->nprov; i++) {
		uint32_t prid = b2i(4, reader->prid[i]);
		if (prid == provid || ( (reader->typ == R_CAMD35 || reader->typ == R_CS378X) && (prid & 0xFFFF) == (provid & 0xFFFF) )) {
			cs_debug_mask(D_EMM, "emm reader %s provider match %04X:%06X", reader->label, caid, provid);
			return 1;
		}
	}
	cs_debug_mask(D_EMM, "emm reader %s skip provider %04X:%06X", reader->label, caid, provid);
	return 0;
}
