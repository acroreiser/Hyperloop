/*
 *   fs/cifs/smb2pdu.c
 *
 *   Copyright (C) International Business Machines  Corp., 2009, 2012
 *                 Etersoft, 2012
 *   Author(s): Steve French (sfrench@us.ibm.com)
 *              Pavel Shilovsky (pshilovsky@samba.org) 2012
 *
 *   Contains the routines for constructing the SMB2 PDUs themselves
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

 /* SMB2 PDU handling routines here - except for leftovers (eg session setup) */
 /* Note that there are handle based routines which must be		      */
 /* treated slightly differently for reconnection purposes since we never     */
 /* want to reuse a stale file handle and only the caller knows the file info */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/vfs.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>
#include <linux/xattr.h>
#include "smb2pdu.h"
#include "cifsglob.h"
#include "cifsacl.h"
#include "cifsproto.h"
#include "smb2proto.h"
#include "cifs_unicode.h"
#include "cifs_debug.h"
#include "ntlmssp.h"
#include "smb2status.h"
#include "smb2glob.h"
#include "cifspdu.h"

/*
 *  The following table defines the expected "StructureSize" of SMB2 requests
 *  in order by SMB2 command.  This is similar to "wct" in SMB/CIFS requests.
 *
 *  Note that commands are defined in smb2pdu.h in le16 but the array below is
 *  indexed by command in host byte order.
 */
static const int smb2_req_struct_sizes[NUMBER_OF_SMB2_COMMANDS] = {
	/* SMB2_NEGOTIATE */ 36,
	/* SMB2_SESSION_SETUP */ 25,
	/* SMB2_LOGOFF */ 4,
	/* SMB2_TREE_CONNECT */	9,
	/* SMB2_TREE_DISCONNECT */ 4,
	/* SMB2_CREATE */ 57,
	/* SMB2_CLOSE */ 24,
	/* SMB2_FLUSH */ 24,
	/* SMB2_READ */	49,
	/* SMB2_WRITE */ 49,
	/* SMB2_LOCK */	48,
	/* SMB2_IOCTL */ 57,
	/* SMB2_CANCEL */ 4,
	/* SMB2_ECHO */ 4,
	/* SMB2_QUERY_DIRECTORY */ 33,
	/* SMB2_CHANGE_NOTIFY */ 32,
	/* SMB2_QUERY_INFO */ 41,
	/* SMB2_SET_INFO */ 33,
	/* SMB2_OPLOCK_BREAK */ 24 /* BB this is 36 for LEASE_BREAK variant */
};


static void
smb2_hdr_assemble(struct smb2_hdr *hdr, __le16 smb2_cmd /* command */ ,
		  const struct cifs_tcon *tcon)
{
	struct smb2_pdu *pdu = (struct smb2_pdu *)hdr;
	char *temp = (char *)hdr;
	/* lookup word count ie StructureSize from table */
	__u16 parmsize = smb2_req_struct_sizes[le16_to_cpu(smb2_cmd)];

	/*
	 * smaller than SMALL_BUFFER_SIZE but bigger than fixed area of
	 * largest operations (Create)
	 */
	memset(temp, 0, 256);

	/* Note this is only network field converted to big endian */
	hdr->smb2_buf_length = cpu_to_be32(parmsize + sizeof(struct smb2_hdr)
			- 4 /*  RFC 1001 length field itself not counted */);

	hdr->ProtocolId[0] = 0xFE;
	hdr->ProtocolId[1] = 'S';
	hdr->ProtocolId[2] = 'M';
	hdr->ProtocolId[3] = 'B';
	hdr->StructureSize = cpu_to_le16(64);
	hdr->Command = smb2_cmd;
	hdr->CreditRequest = cpu_to_le16(2); /* BB make this dynamic */
	hdr->ProcessId = cpu_to_le32((__u16)current->tgid);

	if (!tcon)
		goto out;

	hdr->TreeId = tcon->tid;
	/* Uid is not converted */
	if (tcon->ses)
		hdr->SessionId = tcon->ses->Suid;
	/* BB check following DFS flags BB */
	/* BB do we have to add check for SHI1005_FLAGS_DFS_ROOT too? */
	if (tcon->share_flags & SHI1005_FLAGS_DFS)
		hdr->Flags |= SMB2_FLAGS_DFS_OPERATIONS;
	/* BB how does SMB2 do case sensitive? */
	/* if (tcon->nocase)
		hdr->Flags |= SMBFLG_CASELESS; */
	if (tcon->ses && tcon->ses->server &&
	    (tcon->ses->server->sec_mode & SECMODE_SIGN_REQUIRED))
		hdr->Flags |= SMB2_FLAGS_SIGNED;
out:
	pdu->StructureSize2 = cpu_to_le16(parmsize);
	return;
}

static int
smb2_reconnect(__le16 smb2_command, struct cifs_tcon *tcon)
{
	int rc = 0;
	struct nls_table *nls_codepage;
	struct cifs_ses *ses;
	struct TCP_Server_Info *server;

	/*
	 * SMB2s NegProt, SessSetup, Logoff do not have tcon yet so
	 * check for tcp and smb session status done differently
	 * for those three - in the calling routine.
	 */
	if (tcon == NULL)
		return rc;

	if (smb2_command == SMB2_TREE_CONNECT)
		return rc;

	if (tcon->tidStatus == CifsExiting) {
		/*
		 * only tree disconnect, open, and write,
		 * (and ulogoff which does not have tcon)
		 * are allowed as we start force umount.
		 */
		if ((smb2_command != SMB2_WRITE) &&
		   (smb2_command != SMB2_CREATE) &&
		   (smb2_command != SMB2_TREE_DISCONNECT)) {
			cifs_dbg(FYI, "can not send cmd %d while umounting\n",
				 smb2_command);
			return -ENODEV;
		}
	}
	if ((!tcon->ses) || (tcon->ses->status == CifsExiting) ||
	    (!tcon->ses->server))
		return -EIO;

	ses = tcon->ses;
	server = ses->server;

	/*
	 * Give demultiplex thread up to 10 seconds to reconnect, should be
	 * greater than cifs socket timeout which is 7 seconds
	 */
	while (server->tcpStatus == CifsNeedReconnect) {
		/*
		 * Return to caller for TREE_DISCONNECT and LOGOFF and CLOSE
		 * here since they are implicitly done when session drops.
		 */
		switch (smb2_command) {
		/*
		 * BB Should we keep oplock break and add flush to exceptions?
		 */
		case SMB2_TREE_DISCONNECT:
		case SMB2_CANCEL:
		case SMB2_CLOSE:
		case SMB2_OPLOCK_BREAK:
			return -EAGAIN;
		}

		wait_event_interruptible_timeout(server->response_q,
			(server->tcpStatus != CifsNeedReconnect), 10 * HZ);

		/* are we still trying to reconnect? */
		if (server->tcpStatus != CifsNeedReconnect)
			break;

		/*
		 * on "soft" mounts we wait once. Hard mounts keep
		 * retrying until process is killed or server comes
		 * back on-line
		 */
		if (!tcon->retry) {
			cifs_dbg(FYI, "gave up waiting on reconnect in smb_init\n");
			return -EHOSTDOWN;
		}
	}

	if (!tcon->ses->need_reconnect && !tcon->need_reconnect)
		return rc;

	nls_codepage = load_nls_default();

	/*
	 * need to prevent multiple threads trying to simultaneously reconnect
	 * the same SMB session
	 */
	mutex_lock(&tcon->ses->session_mutex);
	rc = cifs_negotiate_protocol(0, tcon->ses);
	if (!rc && tcon->ses->need_reconnect)
		rc = cifs_setup_session(0, tcon->ses, nls_codepage);

	if (rc || !tcon->need_reconnect) {
		mutex_unlock(&tcon->ses->session_mutex);
		goto out;
	}

	cifs_mark_open_files_invalid(tcon);
	rc = SMB2_tcon(0, tcon->ses, tcon->treeName, tcon, nls_codepage);
	mutex_unlock(&tcon->ses->session_mutex);
	cifs_dbg(FYI, "reconnect tcon rc = %d\n", rc);
	if (rc)
		goto out;
	atomic_inc(&tconInfoReconnectCount);
	/*
	 * BB FIXME add code to check if wsize needs update due to negotiated
	 * smb buffer size shrinking.
	 */
out:
	/*
	 * Check if handle based operation so we know whether we can continue
	 * or not without returning to caller to reset file handle.
	 */
	/*
	 * BB Is flush done by server on drop of tcp session? Should we special
	 * case it and skip above?
	 */
	switch (smb2_command) {
	case SMB2_FLUSH:
	case SMB2_READ:
	case SMB2_WRITE:
	case SMB2_LOCK:
	case SMB2_IOCTL:
	case SMB2_QUERY_DIRECTORY:
	case SMB2_CHANGE_NOTIFY:
	case SMB2_QUERY_INFO:
	case SMB2_SET_INFO:
		rc = -EAGAIN;
	}
	unload_nls(nls_codepage);
	return rc;
}

/*
 * Allocate and return pointer to an SMB request hdr, and set basic
 * SMB information in the SMB header. If the return code is zero, this
 * function must have filled in request_buf pointer.
 */
static int
small_smb2_init(__le16 smb2_command, struct cifs_tcon *tcon,
		void **request_buf)
{
	int rc = 0;

	rc = smb2_reconnect(smb2_command, tcon);
	if (rc)
		return rc;

	/* BB eventually switch this to SMB2 specific small buf size */
	*request_buf = cifs_small_buf_get();
	if (*request_buf == NULL) {
		/* BB should we add a retry in here if not a writepage? */
		return -ENOMEM;
	}

	smb2_hdr_assemble((struct smb2_hdr *) *request_buf, smb2_command, tcon);

	if (tcon != NULL) {
#ifdef CONFIG_CIFS_STATS2
		uint16_t com_code = le16_to_cpu(smb2_command);
		cifs_stats_inc(&tcon->stats.smb2_stats.smb2_com_sent[com_code]);
#endif
		cifs_stats_inc(&tcon->num_smbs_sent);
	}

	return rc;
}

static void
free_rsp_buf(int resp_buftype, void *rsp)
{
	if (resp_buftype == CIFS_SMALL_BUFFER)
		cifs_small_buf_release(rsp);
	else if (resp_buftype == CIFS_LARGE_BUFFER)
		cifs_buf_release(rsp);
}


/*
 *
 *	SMB2 Worker functions follow:
 *
 *	The general structure of the worker functions is:
 *	1) Call smb2_init (assembles SMB2 header)
 *	2) Initialize SMB2 command specific fields in fixed length area of SMB
 *	3) Call smb_sendrcv2 (sends request on socket and waits for response)
 *	4) Decode SMB2 command specific fields in the fixed length area
 *	5) Decode variable length data area (if any for this SMB2 command type)
 *	6) Call free smb buffer
 *	7) return
 *
 */

int
SMB2_negotiate(const unsigned int xid, struct cifs_ses *ses)
{
	struct smb2_negotiate_req *req;
	struct smb2_negotiate_rsp *rsp;
	struct kvec iov[1];
	int rc = 0;
	int resp_buftype;
	struct TCP_Server_Info *server;
	unsigned int sec_flags;
	u16 temp = 0;
	int blob_offset, blob_length;
	char *security_blob;
	int flags = CIFS_NEG_OP;

	cifs_dbg(FYI, "Negotiate protocol\n");

	if (ses->server)
		server = ses->server;
	else {
		rc = -EIO;
		return rc;
	}

	rc = small_smb2_init(SMB2_NEGOTIATE, NULL, (void **) &req);
	if (rc)
		return rc;

	/* if any of auth flags (ie not sign or seal) are overriden use them */
	if (ses->overrideSecFlg & (~(CIFSSEC_MUST_SIGN | CIFSSEC_MUST_SEAL)))
		sec_flags = ses->overrideSecFlg;  /* BB FIXME fix sign flags?*/
	else /* if override flags set only sign/seal OR them with global auth */
		sec_flags = global_secflags | ses->overrideSecFlg;

	cifs_dbg(FYI, "sec_flags 0x%x\n", sec_flags);

	req->hdr.SessionId = 0;

	req->Dialects[0] = cpu_to_le16(ses->server->vals->protocol_id);

	req->DialectCount = cpu_to_le16(1); /* One vers= at a time for now */
	inc_rfc1001_len(req, 2);

	/* only one of SMB2 signing flags may be set in SMB2 request */
	if ((sec_flags & CIFSSEC_MUST_SIGN) == CIFSSEC_MUST_SIGN)
		temp = SMB2_NEGOTIATE_SIGNING_REQUIRED;
	else if (sec_flags & CIFSSEC_MAY_SIGN) /* MAY_SIGN is a single flag */
		temp = SMB2_NEGOTIATE_SIGNING_ENABLED;

	req->SecurityMode = cpu_to_le16(temp);

	req->Capabilities = cpu_to_le32(ses->server->vals->req_capabilities);

	memcpy(req->ClientGUID, cifs_client_guid, SMB2_CLIENT_GUID_SIZE);

	iov[0].iov_base = (char *)req;
	/* 4 for rfc1002 length field */
	iov[0].iov_len = get_rfc1002_length(req) + 4;

	rc = SendReceive2(xid, ses, iov, 1, &resp_buftype, flags);

	rsp = (struct smb2_negotiate_rsp *)iov[0].iov_base;
	/*
	 * No tcon so can't do
	 * cifs_stats_inc(&tcon->stats.smb2_stats.smb2_com_fail[SMB2...]);
	 */
	if (rc != 0)
		goto neg_exit;

	cifs_dbg(FYI, "mode 0x%x\n", rsp->SecurityMode);

	/* BB we may eventually want to match the negotiated vs. requested
	   dialect, even though we are only requesting one at a time */
	if (rsp->DialectRevision == cpu_to_le16(SMB20_PROT_ID))
		cifs_dbg(FYI, "negotiated smb2.0 dialect\n");
	else if (rsp->DialectRevision == cpu_to_le16(SMB21_PROT_ID))
		cifs_dbg(FYI, "negotiated smb2.1 dialect\n");
	else if (rsp->DialectRevision == cpu_to_le16(SMB30_PROT_ID))
		cifs_dbg(FYI, "negotiated smb3.0 dialect\n");
	else {
		cifs_dbg(VFS, "Illegal dialect returned by server %d\n",
			 le16_to_cpu(rsp->DialectRevision));
		rc = -EIO;
		goto neg_exit;
	}
	server->dialect = le16_to_cpu(rsp->DialectRevision);

	server->maxBuf = le32_to_cpu(rsp->MaxTransactSize);
	/* set it to the maximum buffer size value we can send with 1 credit */
	server->maxBuf = min_t(unsigned int, le32_to_cpu(rsp->MaxTransactSize),
			       SMB2_MAX_BUFFER_SIZE);
	server->max_read = le32_to_cpu(rsp->MaxReadSize);
	server->max_write = le32_to_cpu(rsp->MaxWriteSize);
	/* BB Do we need to validate the SecurityMode? */
	server->sec_mode = le16_to_cpu(rsp->SecurityMode);
	server->capabilities = le32_to_cpu(rsp->Capabilities);
	/* Internal types */
	server->capabilities |= SMB2_NT_FIND | SMB2_LARGE_FILES;

	security_blob = smb2_get_data_area_len(&blob_offset, &blob_length,
					       &rsp->hdr);
	if (blob_length == 0) {
		cifs_dbg(VFS, "missing security blob on negprot\n");
		rc = -EIO;
		goto neg_exit;
	}

	cifs_dbg(FYI, "sec_flags 0x%x\n", sec_flags);
	if ((sec_flags & CIFSSEC_MUST_SIGN) == CIFSSEC_MUST_SIGN) {
		cifs_dbg(FYI, "Signing required\n");
		if (!(server->sec_mode & (SMB2_NEGOTIATE_SIGNING_REQUIRED |
		      SMB2_NEGOTIATE_SIGNING_ENABLED))) {
			cifs_dbg(VFS, "signing required but server lacks support\n");
			rc = -EOPNOTSUPP;
			goto neg_exit;
		}
		server->sec_mode |= SECMODE_SIGN_REQUIRED;
	} else if (sec_flags & CIFSSEC_MAY_SIGN) {
		cifs_dbg(FYI, "Signing optional\n");
		if (server->sec_mode & SMB2_NEGOTIATE_SIGNING_REQUIRED) {
			cifs_dbg(FYI, "Server requires signing\n");
			server->sec_mode |= SECMODE_SIGN_REQUIRED;
		} else {
			server->sec_mode &=
				~(SECMODE_SIGN_ENABLED | SECMODE_SIGN_REQUIRED);
		}
	} else {
		cifs_dbg(FYI, "Signing disabled\n");
		if (server->sec_mode & SMB2_NEGOTIATE_SIGNING_REQUIRED) {
			cifs_dbg(VFS, "Server requires packet signing to be enabled in /proc/fs/cifs/SecurityFlags\n");
			rc = -EOPNOTSUPP;
			goto neg_exit;
		}
		server->sec_mode &=
			~(SECMODE_SIGN_ENABLED | SECMODE_SIGN_REQUIRED);
	}

#ifdef CONFIG_SMB2_ASN1  /* BB REMOVEME when updated asn1.c ready */
	rc = decode_neg_token_init(security_blob, blob_length,
				   &server->sec_type);
	if (rc == 1)
		rc = 0;
	else if (rc == 0) {
		rc = -EIO;
		goto neg_exit;
	}
#endif

neg_exit:
	free_rsp_buf(resp_buftype, rsp);
	return rc;
}

int
SMB2_sess_setup(const unsigned int xid, struct cifs_ses *ses,
		const struct nls_table *nls_cp)
{
	struct smb2_sess_setup_req *req;
	struct smb2_sess_setup_rsp *rsp = NULL;
	struct kvec iov[2];
	int rc = 0;
	int resp_buftype;
	__le32 phase = NtLmNegotiate; /* NTLMSSP, if needed, is multistage */
	struct TCP_Server_Info *server;
	unsigned int sec_flags;
	u8 temp = 0;
	u16 blob_length = 0;
	char *security_blob;
	char *ntlmssp_blob = NULL;
	bool use_spnego = false; /* else use raw ntlmssp */

	cifs_dbg(FYI, "Session Setup\n");

	if (ses->server)
		server = ses->server;
	else {
		rc = -EIO;
		return rc;
	}

	/*
	 * If memory allocation is successful, caller of this function
	 * frees it.
	 */
	ses->ntlmssp = kmalloc(sizeof(struct ntlmssp_auth), GFP_KERNEL);
	if (!ses->ntlmssp)
		return -ENOMEM;

	ses->server->secType = RawNTLMSSP;

ssetup_ntlmssp_authenticate:
	if (phase == NtLmChallenge)
		phase = NtLmAuthenticate; /* if ntlmssp, now final phase */

	rc = small_smb2_init(SMB2_SESSION_SETUP, NULL, (void **) &req);
	if (rc)
		return rc;

	/* if any of auth flags (ie not sign or seal) are overriden use them */
	if (ses->overrideSecFlg & (~(CIFSSEC_MUST_SIGN | CIFSSEC_MUST_SEAL)))
		sec_flags = ses->overrideSecFlg;  /* BB FIXME fix sign flags?*/
	else /* if override flags set only sign/seal OR them with global auth */
		sec_flags = global_secflags | ses->overrideSecFlg;

	cifs_dbg(FYI, "sec_flags 0x%x\n", sec_flags);

	req->hdr.SessionId = 0; /* First session, not a reauthenticate */
	req->VcNumber = 0; /* MBZ */
	/* to enable echos and oplocks */
	req->hdr.CreditRequest = cpu_to_le16(3);

	/* only one of SMB2 signing flags may be set in SMB2 request */
	if ((sec_flags & CIFSSEC_MUST_SIGN) == CIFSSEC_MUST_SIGN)
		temp = SMB2_NEGOTIATE_SIGNING_REQUIRED;
	else if (ses->server->sec_mode & SMB2_NEGOTIATE_SIGNING_REQUIRED)
		temp = SMB2_NEGOTIATE_SIGNING_REQUIRED;
	else if (sec_flags & CIFSSEC_MAY_SIGN) /* MAY_SIGN is a single flag */
		temp = SMB2_NEGOTIATE_SIGNING_ENABLED;

	req->SecurityMode = temp;
	req->Capabilities = 0;
	req->Channel = 0; /* MBZ */

	iov[0].iov_base = (char *)req;
	/* 4 for rfc1002 length field and 1 for pad */
	iov[0].iov_len = get_rfc1002_length(req) + 4 - 1;
	if (phase == NtLmNegotiate) {
		ntlmssp_blob = kmalloc(sizeof(struct _NEGOTIATE_MESSAGE),
				       GFP_KERNEL);
		if (ntlmssp_blob == NULL) {
			rc = -ENOMEM;
			goto ssetup_exit;
		}
		build_ntlmssp_negotiate_blob(ntlmssp_blob, ses);
		if (use_spnego) {
			/* blob_length = build_spnego_ntlmssp_blob(
					&security_blob,
					sizeof(struct _NEGOTIATE_MESSAGE),
					ntlmssp_blob); */
			/* BB eventually need to add this */
			cifs_dbg(VFS, "spnego not supported for SMB2 yet\n");
			rc = -EOPNOTSUPP;
			kfree(ntlmssp_blob);
			goto ssetup_exit;
		} else {
			blob_length = sizeof(struct _NEGOTIATE_MESSAGE);
			/* with raw NTLMSSP we don't encapsulate in SPNEGO */
			security_blob = ntlmssp_blob;
		}
	} else if (phase == NtLmAuthenticate) {
		req->hdr.SessionId = ses->Suid;
		ntlmssp_blob = kzalloc(sizeof(struct _NEGOTIATE_MESSAGE) + 500,
				       GFP_KERNEL);
		if (ntlmssp_blob == NULL) {
			rc = -ENOMEM;
			goto ssetup_exit;
		}
		rc = build_ntlmssp_auth_blob(ntlmssp_blob, &blob_length, ses,
					     nls_cp);
		if (rc) {
			cifs_dbg(FYI, "build_ntlmssp_auth_blob failed %d\n",
				 rc);
			goto ssetup_exit; /* BB double check error handling */
		}
		if (use_spnego) {
			/* blob_length = build_spnego_ntlmssp_blob(
							&security_blob,
							blob_length,
							ntlmssp_blob); */
			cifs_dbg(VFS, "spnego not supported for SMB2 yet\n");
			rc = -EOPNOTSUPP;
			kfree(ntlmssp_blob);
			goto ssetup_exit;
		} else {
			security_blob = ntlmssp_blob;
		}
	} else {
		cifs_dbg(VFS, "illegal ntlmssp phase\n");
		rc = -EIO;
		goto ssetup_exit;
	}

	/* Testing shows that buffer offset must be at location of Buffer[0] */
	req->SecurityBufferOffset =
				cpu_to_le16(sizeof(struct smb2_sess_setup_req) -
					    1 /* pad */ - 4 /* rfc1001 len */);
	req->SecurityBufferLength = cpu_to_le16(blob_length);
	iov[1].iov_base = security_blob;
	iov[1].iov_len = blob_length;

	inc_rfc1001_len(req, blob_length - 1 /* pad */);

	/* BB add code to build os and lm fields */

	rc = SendReceive2(xid, ses, iov, 2, &resp_buftype,
			  CIFS_LOG_ERROR | CIFS_NEG_OP);

	kfree(security_blob);
	rsp = (struct smb2_sess_setup_rsp *)iov[0].iov_base;
	if (resp_buftype != CIFS_NO_BUFFER &&
	    rsp->hdr.Status == STATUS_MORE_PROCESSING_REQUIRED) {
		if (phase != NtLmNegotiate) {
			cifs_dbg(VFS, "Unexpected more processing error\n");
			goto ssetup_exit;
		}
		if (offsetof(struct smb2_sess_setup_rsp, Buffer) - 4 !=
				le16_to_cpu(rsp->SecurityBufferOffset)) {
			cifs_dbg(VFS, "Invalid security buffer offset %d\n",
				 le16_to_cpu(rsp->SecurityBufferOffset));
			rc = -EIO;
			goto ssetup_exit;
		}

		/* NTLMSSP Negotiate sent now processing challenge (response) */
		phase = NtLmChallenge; /* process ntlmssp challenge */
		rc = 0; /* MORE_PROCESSING is not an error here but expected */
		ses->Suid = rsp->hdr.SessionId;
		rc = decode_ntlmssp_challenge(rsp->Buffer,
				le16_to_cpu(rsp->SecurityBufferLength), ses);
	}

	/*
	 * BB eventually add code for SPNEGO decoding of NtlmChallenge blob,
	 * but at least the raw NTLMSSP case works.
	 */
	/*
	 * No tcon so can't do
	 * cifs_stats_inc(&tcon->stats.smb2_stats.smb2_com_fail[SMB2...]);
	 */
	if (rc != 0)
		goto ssetup_exit;

	ses->session_flags = le16_to_cpu(rsp->SessionFlags);
ssetup_exit:
	free_rsp_buf(resp_buftype, rsp);

	/* if ntlmssp, and negotiate succeeded, proceed to authenticate phase */
	if ((phase == NtLmChallenge) && (rc == 0))
		goto ssetup_ntlmssp_authenticate;
	return rc;
}

int
SMB2_logoff(const unsigned int xid, struct cifs_ses *ses)
{
	struct smb2_logoff_req *req; /* response is also trivial struct */
	int rc = 0;
	struct TCP_Server_Info *server;

	cifs_dbg(FYI, "disconnect session %p\n", ses);

	if (ses && (ses->server))
		server = ses->server;
	else
		return -EIO;

	rc = small_smb2_init(SMB2_LOGOFF, NULL, (void **) &req);
	if (rc)
		return rc;

	 /* since no tcon, smb2_init can not do this, so do here */
	req->hdr.SessionId = ses->Suid;
	if (server->sec_mode & SECMODE_SIGN_REQUIRED)
		req->hdr.Flags |= SMB2_FLAGS_SIGNED;

	rc = SendReceiveNoRsp(xid, ses, (char *) &req->hdr, 0);
	/*
	 * No tcon so can't do
	 * cifs_stats_inc(&tcon->stats.smb2_stats.smb2_com_fail[SMB2...]);
	 */
	return rc;
}

static inline void cifs_stats_fail_inc(struct cifs_tcon *tcon, uint16_t code)
{
	cifs_stats_inc(&tcon->stats.smb2_stats.smb2_com_failed[code]);
}

#define MAX_SHARENAME_LENGTH (255 /* server */ + 80 /* share */ + 1 /* NULL */)

int
SMB2_tcon(const unsigned int xid, struct cifs_ses *ses, const char *tree,
	  struct cifs_tcon *tcon, const struct nls_table *cp)
{
	struct smb2_tree_connect_req *req;
	struct smb2_tree_connect_rsp *rsp = NULL;
	struct kvec iov[2];
	int rc = 0;
	int resp_buftype;
	int unc_path_len;
	struct TCP_Server_Info *server;
	__le16 *unc_path = NULL;

	cifs_dbg(FYI, "TCON\n");

	if ((ses->server) && tree)
		server = ses->server;
	else
		return -EIO;

	unc_path = kmalloc(MAX_SHARENAME_LENGTH * 2, GFP_KERNEL);
	if (unc_path == NULL)
		return -ENOMEM;

	unc_path_len = cifs_strtoUTF16(unc_path, tree, strlen(tree), cp) + 1;
	unc_path_len *= 2;
	if (unc_path_len < 2) {
		kfree(unc_path);
		return -EINVAL;
	}

	/* SMB2 TREE_CONNECT request must be called with TreeId == 0 */
	if (tcon)
		tcon->tid = 0;

	rc = small_smb2_init(SMB2_TREE_CONNECT, tcon, (void **) &req);
	if (rc) {
		kfree(unc_path);
		return rc;
	}

	if (tcon == NULL) {
		/* since no tcon, smb2_init can not do this, so do here */
		req->hdr.SessionId = ses->Suid;
		/* if (ses->server->sec_mode & SECMODE_SIGN_REQUIRED)
			req->hdr.Flags |= SMB2_FLAGS_SIGNED; */
	}

	iov[0].iov_base = (char *)req;
	/* 4 for rfc1002 length field and 1 for pad */
	iov[0].iov_len = get_rfc1002_length(req) + 4 - 1;

	/* Testing shows that buffer offset must be at location of Buffer[0] */
	req->PathOffset = cpu_to_le16(sizeof(struct smb2_tree_connect_req)
			- 1 /* pad */ - 4 /* do not count rfc1001 len field */);
	req->PathLength = cpu_to_le16(unc_path_len - 2);
	iov[1].iov_base = unc_path;
	iov[1].iov_len = unc_path_len;

	inc_rfc1001_len(req, unc_path_len - 1 /* pad */);

	rc = SendReceive2(xid, ses, iov, 2, &resp_buftype, 0);
	rsp = (struct smb2_tree_connect_rsp *)iov[0].iov_base;

	if (rc != 0) {
		if (tcon) {
			cifs_stats_fail_inc(tcon, SMB2_TREE_CONNECT_HE);
			tcon->need_reconnect = true;
		}
		goto tcon_error_exit;
	}

	if (tcon == NULL) {
		ses->ipc_tid = rsp->hdr.TreeId;
		goto tcon_exit;
	}

	if (rsp->ShareType & SMB2_SHARE_TYPE_DISK)
		cifs_dbg(FYI, "connection to disk share\n");
	else if (rsp->ShareType & SMB2_SHARE_TYPE_PIPE) {
		tcon->ipc = true;
		cifs_dbg(FYI, "connection to pipe share\n");
	} else if (rsp->ShareType & SMB2_SHARE_TYPE_PRINT) {
		tcon->print = true;
		cifs_dbg(FYI, "connection to printer\n");
	} else {
		cifs_dbg(VFS, "unknown share type %d\n", rsp->ShareType);
		rc = -EOPNOTSUPP;
		goto tcon_error_exit;
	}

	tcon->share_flags = le32_to_cpu(rsp->ShareFlags);
	tcon->maximal_access = le32_to_cpu(rsp->MaximalAccess);
	tcon->tidStatus = CifsGood;
	tcon->need_reconnect = false;
	tcon->tid = rsp->hdr.TreeId;
	strncpy(tcon->treeName, tree, MAX_TREE_SIZE);

	if ((rsp->Capabilities & SMB2_SHARE_CAP_DFS) &&
	    ((tcon->share_flags & SHI1005_FLAGS_DFS) == 0))
		cifs_dbg(VFS, "DFS capability contradicts DFS flag\n");

tcon_exit:
	free_rsp_buf(resp_buftype, rsp);
	kfree(unc_path);
	return rc;

tcon_error_exit:
	if (rsp->hdr.Status == STATUS_BAD_NETWORK_NAME) {
		cifs_dbg(VFS, "BAD_NETWORK_NAME: %s\n", tree);
	}
	goto tcon_exit;
}

int
SMB2_tdis(const unsigned int xid, struct cifs_tcon *tcon)
{
	struct smb2_tree_disconnect_req *req; /* response is trivial */
	int rc = 0;
	struct TCP_Server_Info *server;
	struct cifs_ses *ses = tcon->ses;

	cifs_dbg(FYI, "Tree Disconnect\n");

	if (ses && (ses->server))
		server = ses->server;
	else
		return -EIO;

	if ((tcon->need_reconnect) || (tcon->ses->need_reconnect))
		return 0;

	rc = small_smb2_init(SMB2_TREE_DISCONNECT, tcon, (void **) &req);
	if (rc)
		return rc;

	rc = SendReceiveNoRsp(xid, ses, (char *)&req->hdr, 0);
	if (rc)
		cifs_stats_fail_inc(tcon, SMB2_TREE_DISCONNECT_HE);

	return rc;
}

static struct create_lease *
create_lease_buf(u8 *lease_key, u8 oplock)
{
	struct create_lease *buf;

	buf = kzalloc(sizeof(struct create_lease), GFP_KERNEL);
	if (!buf)
		return NULL;

	buf->lcontext.LeaseKeyLow = cpu_to_le64(*((u64 *)lease_key));
	buf->lcontext.LeaseKeyHigh = cpu_to_le64(*((u64 *)(lease_key + 8)));
	if (oplock == SMB2_OPLOCK_LEVEL_EXCLUSIVE)
		buf->lcontext.LeaseState = SMB2_LEASE_WRITE_CACHING |
					   SMB2_LEASE_READ_CACHING;
	else if (oplock == SMB2_OPLOCK_LEVEL_II)
		buf->lcontext.LeaseState = SMB2_LEASE_READ_CACHING;
	else if (oplock == SMB2_OPLOCK_LEVEL_BATCH)
		buf->lcontext.LeaseState = SMB2_LEASE_HANDLE_CACHING |
					   SMB2_LEASE_READ_CACHING |
					   SMB2_LEASE_WRITE_CACHING;

	buf->ccontext.DataOffset = cpu_to_le16(offsetof
					(struct create_lease, lcontext));
	buf->ccontext.DataLength = cpu_to_le32(sizeof(struct lease_context));
	buf->ccontext.NameOffset = cpu_to_le16(offsetof
				(struct create_lease, Name));
	buf->ccontext.NameLength = cpu_to_le16(4);
	buf->Name[0] = 'R';
	buf->Name[1] = 'q';
	buf->Name[2] = 'L';
	buf->Name[3] = 's';
	return buf;
}

static struct create_durable *
create_durable_buf(void)
{
	struct create_durable *buf;

	buf = kzalloc(sizeof(struct create_durable), GFP_KERNEL);
	if (!buf)
		return NULL;

	buf->ccontext.DataOffset = cpu_to_le16(offsetof
					(struct create_durable, Data));
	buf->ccontext.DataLength = cpu_to_le32(16);
	buf->ccontext.NameOffset = cpu_to_le16(offsetof
				(struct create_durable, Name));
	buf->ccontext.NameLength = cpu_to_le16(4);
	buf->Name[0] = 'D';
	buf->Name[1] = 'H';
	buf->Name[2] = 'n';
	buf->Name[3] = 'Q';
	return buf;
}

static struct create_durable *
create_reconnect_durable_buf(struct cifs_fid *fid)
{
	struct create_durable *buf;

	buf = kzalloc(sizeof(struct create_durable), GFP_KERNEL);
	if (!buf)
		return NULL;

	buf->ccontext.DataOffset = cpu_to_le16(offsetof
					(struct create_durable, Data));
	buf->ccontext.DataLength = cpu_to_le32(16);
	buf->ccontext.NameOffset = cpu_to_le16(offsetof
				(struct create_durable, Name));
	buf->ccontext.NameLength = cpu_to_le16(4);
	buf->Data.Fid.PersistentFileId = fid->persistent_fid;
	buf->Data.Fid.VolatileFileId = fid->volatile_fid;
	buf->Name[0] = 'D';
	buf->Name[1] = 'H';
	buf->Name[2] = 'n';
	buf->Name[3] = 'C';
	return buf;
}

static __u8
parse_lease_state(struct smb2_create_rsp *rsp)
{
	char *data_offset;
	struct create_lease *lc;
	bool found = false;
	unsigned int next = 0;
	char *name;

	data_offset = (char *)rsp + 4 + le32_to_cpu(rsp->CreateContextsOffset);
	lc = (struct create_lease *)data_offset;
	do {
		lc = (struct create_lease *)((char *)lc + next);
		name = le16_to_cpu(lc->ccontext.NameOffset) + (char *)lc;
		if (le16_to_cpu(lc->ccontext.NameLength) != 4 ||
		    strncmp(name, "RqLs", 4)) {
			next = le32_to_cpu(lc->ccontext.Next);
			continue;
		}
		if (lc->lcontext.LeaseFlags & SMB2_LEASE_FLAG_BREAK_IN_PROGRESS)
			return SMB2_OPLOCK_LEVEL_NOCHANGE;
		found = true;
		break;
	} while (next != 0);

	if (!found)
		return 0;

	return smb2_map_lease_to_oplock(lc->lcontext.LeaseState);
}

static int
add_lease_context(struct kvec *iov, unsigned int *num_iovec, __u8 *oplock)
{
	struct smb2_create_req *req = iov[0].iov_base;
	unsigned int num = *num_iovec;

	iov[num].iov_base = create_lease_buf(oplock+1, *oplock);
	if (iov[num].iov_base == NULL)
		return -ENOMEM;
	iov[num].iov_len = sizeof(struct create_lease);
	req->RequestedOplockLevel = SMB2_OPLOCK_LEVEL_LEASE;
	if (!req->CreateContextsOffset)
		req->CreateContextsOffset = cpu_to_le32(
				sizeof(struct smb2_create_req) - 4 +
				iov[num - 1].iov_len);
	req->CreateContextsLength = cpu_to_le32(
				le32_to_cpu(req->CreateContextsLength) +
				sizeof(struct create_lease));
	inc_rfc1001_len(&req->hdr, sizeof(struct create_lease));
	*num_iovec = num + 1;
	return 0;
}

static int
add_durable_context(struct kvec *iov, unsigned int *num_iovec,
		    struct cifs_open_parms *oparms)
{
	struct smb2_create_req *req = iov[0].iov_base;
	unsigned int num = *num_iovec;

	if (oparms->reconnect) {
		iov[num].iov_base = create_reconnect_durable_buf(oparms->fid);
		/* indicate that we don't need to relock the file */
		oparms->reconnect = false;
	} else
		iov[num].iov_base = create_durable_buf();
	if (iov[num].iov_base == NULL)
		return -ENOMEM;
	iov[num].iov_len = sizeof(struct create_durable);
	if (!req->CreateContextsOffset)
		req->CreateContextsOffset =
			cpu_to_le32(sizeof(struct smb2_create_req) - 4 +
								iov[1].iov_len);
	req->CreateContextsLength =
			cpu_to_le32(le32_to_cpu(req->CreateContextsLength) +
						sizeof(struct create_durable));
	inc_rfc1001_len(&req->hdr, sizeof(struct create_durable));
	*num_iovec = num + 1;
	return 0;
}

int
SMB2_open(const unsigned int xid, struct cifs_open_parms *oparms, __le16 *path,
	  __u8 *oplock, struct smb2_file_all_info *buf)
{
	struct smb2_create_req *req;
	struct smb2_create_rsp *rsp;
	struct TCP_Server_Info *server;
	struct cifs_tcon *tcon = oparms->tcon;
	struct cifs_ses *ses = tcon->ses;
	struct kvec iov[4];
	int resp_buftype;
	int uni_path_len;
	__le16 *copy_path = NULL;
	int copy_size;
	int rc = 0;
	unsigned int num_iovecs = 2;
	__u32 file_attributes = 0;

	cifs_dbg(FYI, "create/open\n");

	if (ses && (ses->server))
		server = ses->server;
	else
		return -EIO;

	rc = small_smb2_init(SMB2_CREATE, tcon, (void **) &req);
	if (rc)
		return rc;

	if (oparms->create_options & CREATE_OPTION_READONLY)
		file_attributes |= ATTR_READONLY;

	req->ImpersonationLevel = IL_IMPERSONATION;
	req->DesiredAccess = cpu_to_le32(oparms->desired_access);
	/* File attributes ignored on open (used in create though) */
	req->FileAttributes = cpu_to_le32(file_attributes);
	req->ShareAccess = FILE_SHARE_ALL_LE;
	req->CreateDisposition = cpu_to_le32(oparms->disposition);
	req->CreateOptions = cpu_to_le32(oparms->create_options & CREATE_OPTIONS_MASK);
	uni_path_len = (2 * UniStrnlen((wchar_t *)path, PATH_MAX)) + 2;
	/* do not count rfc1001 len field */
	req->NameOffset = cpu_to_le16(sizeof(struct smb2_create_req) - 4);

	iov[0].iov_base = (char *)req;
	/* 4 for rfc1002 length field */
	iov[0].iov_len = get_rfc1002_length(req) + 4;

	/* MUST set path len (NameLength) to 0 opening root of share */
	req->NameLength = cpu_to_le16(uni_path_len - 2);
	/* -1 since last byte is buf[0] which is sent below (path) */
	iov[0].iov_len--;
	if (uni_path_len % 8 != 0) {
		copy_size = uni_path_len / 8 * 8;
		if (copy_size < uni_path_len)
			copy_size += 8;

		copy_path = kzalloc(copy_size, GFP_KERNEL);
		if (!copy_path)
			return -ENOMEM;
		memcpy((char *)copy_path, (const char *)path,
			uni_path_len);
		uni_path_len = copy_size;
		path = copy_path;
	}

	iov[1].iov_len = uni_path_len;
	iov[1].iov_base = path;
	/* -1 since last byte is buf[0] which was counted in smb2_buf_len */
	inc_rfc1001_len(req, uni_path_len - 1);

	if (!server->oplocks)
		*oplock = SMB2_OPLOCK_LEVEL_NONE;

	if (!(tcon->ses->server->capabilities & SMB2_GLOBAL_CAP_LEASING) ||
	    *oplock == SMB2_OPLOCK_LEVEL_NONE)
		req->RequestedOplockLevel = *oplock;
	else {
		rc = add_lease_context(iov, &num_iovecs, oplock);
		if (rc) {
			cifs_small_buf_release(req);
			kfree(copy_path);
			return rc;
		}
	}

	if (*oplock == SMB2_OPLOCK_LEVEL_BATCH) {
		/* need to set Next field of lease context if we request it */
		if (tcon->ses->server->capabilities & SMB2_GLOBAL_CAP_LEASING) {
			struct create_context *ccontext =
			    (struct create_context *)iov[num_iovecs-1].iov_base;
			ccontext->Next = sizeof(struct create_lease);
		}
		rc = add_durable_context(iov, &num_iovecs, oparms);
		if (rc) {
			cifs_small_buf_release(req);
			kfree(copy_path);
			kfree(iov[num_iovecs-1].iov_base);
			return rc;
		}
	}

	rc = SendReceive2(xid, ses, iov, num_iovecs, &resp_buftype, 0);
	rsp = (struct smb2_create_rsp *)iov[0].iov_base;

	if (rc != 0) {
		cifs_stats_fail_inc(tcon, SMB2_CREATE_HE);
		goto creat_exit;
	}

	oparms->fid->persistent_fid = rsp->PersistentFileId;
	oparms->fid->volatile_fid = rsp->VolatileFileId;

	if (buf) {
		memcpy(buf, &rsp->CreationTime, 32);
		buf->AllocationSize = rsp->AllocationSize;
		buf->EndOfFile = rsp->EndofFile;
		buf->Attributes = rsp->FileAttributes;
		buf->NumberOfLinks = cpu_to_le32(1);
		buf->DeletePending = 0;
	}

	if (rsp->OplockLevel == SMB2_OPLOCK_LEVEL_LEASE)
		*oplock = parse_lease_state(rsp);
	else
		*oplock = rsp->OplockLevel;
creat_exit:
	kfree(copy_path);
	free_rsp_buf(resp_buftype, rsp);
	return rc;
}

int
SMB2_close(const unsigned int xid, struct cifs_tcon *tcon,
	   u64 persistent_fid, u64 volatile_fid)
{
	struct smb2_close_req *req;
	struct smb2_close_rsp *rsp;
	struct TCP_Server_Info *server;
	struct cifs_ses *ses = tcon->ses;
	struct kvec iov[1];
	int resp_buftype;
	int rc = 0;

	cifs_dbg(FYI, "Close\n");

	if (ses && (ses->server))
		server = ses->server;
	else
		return -EIO;

	rc = small_smb2_init(SMB2_CLOSE, tcon, (void **) &req);
	if (rc)
		return rc;

	req->PersistentFileId = persistent_fid;
	req->VolatileFileId = volatile_fid;

	iov[0].iov_base = (char *)req;
	/* 4 for rfc1002 length field */
	iov[0].iov_len = get_rfc1002_length(req) + 4;

	rc = SendReceive2(xid, ses, iov, 1, &resp_buftype, 0);
	rsp = (struct smb2_close_rsp *)iov[0].iov_base;

	if (rc != 0) {
		if (tcon)
			cifs_stats_fail_inc(tcon, SMB2_CLOSE_HE);
		goto close_exit;
	}

	/* BB FIXME - decode close response, update inode for caching */

close_exit:
	free_rsp_buf(resp_buftype, rsp);
	return rc;
}

static int
validate_buf(unsigned int offset, unsigned int buffer_length,
	     struct smb2_hdr *hdr, unsigned int min_buf_size)

{
	unsigned int smb_len = be32_to_cpu(hdr->smb2_buf_length);
	char *end_of_smb = smb_len + 4 /* RFC1001 length field */ + (char *)hdr;
	char *begin_of_buf = 4 /* RFC1001 len field */ + offset + (char *)hdr;
	char *end_of_buf = begin_of_buf + buffer_length;


	if (buffer_length < min_buf_size) {
		cifs_dbg(VFS, "buffer length %d smaller than minimum size %d\n",
			 buffer_length, min_buf_size);
		return -EINVAL;
	}

	/* check if beyond RFC1001 maximum length */
	if ((smb_len > 0x7FFFFF) || (buffer_length > 0x7FFFFF)) {
		cifs_dbg(VFS, "buffer length %d or smb length %d too large\n",
			 buffer_length, smb_len);
		return -EINVAL;
	}

	if ((begin_of_buf > end_of_smb) || (end_of_buf > end_of_smb)) {
		cifs_dbg(VFS, "illegal server response, bad offset to data\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * If SMB buffer fields are valid, copy into temporary buffer to hold result.
 * Caller must free buffer.
 */
static int
validate_and_copy_buf(unsigned int offset, unsigned int buffer_length,
		      struct smb2_hdr *hdr, unsigned int minbufsize,
		      char *data)

{
	char *begin_of_buf = 4 /* RFC1001 len field */ + offset + (char *)hdr;
	int rc;

	if (!data)
		return -EINVAL;

	rc = validate_buf(offset, buffer_length, hdr, minbufsize);
	if (rc)
		return rc;

	memcpy(data, begin_of_buf, buffer_length);

	return 0;
}

static int
query_info(const unsigned int xid, struct cifs_tcon *tcon,
	   u64 persistent_fid, u64 volatile_fid, u8 info_class,
	   size_t output_len, size_t min_len, void *data)
{
	struct smb2_query_info_req *req;
	struct smb2_query_info_rsp *rsp = NULL;
	struct kvec iov[2];
	int rc = 0;
	int resp_buftype;
	struct TCP_Server_Info *server;
	struct cifs_ses *ses = tcon->ses;

	cifs_dbg(FYI, "Query Info\n");

	if (ses && (ses->server))
		server = ses->server;
	else
		return -EIO;

	rc = small_smb2_init(SMB2_QUERY_INFO, tcon, (void **) &req);
	if (rc)
		return rc;

	req->InfoType = SMB2_O_INFO_FILE;
	req->FileInfoClass = info_class;
	req->PersistentFileId = persistent_fid;
	req->VolatileFileId = volatile_fid;
	/* 4 for rfc1002 length field and 1 for Buffer */
	req->InputBufferOffset =
		cpu_to_le16(sizeof(struct smb2_query_info_req) - 1 - 4);
	req->OutputBufferLength = cpu_to_le32(output_len);

	iov[0].iov_base = (char *)req;
	/* 4 for rfc1002 length field */
	iov[0].iov_len = get_rfc1002_length(req) + 4;

	rc = SendReceive2(xid, ses, iov, 1, &resp_buftype, 0);
	rsp = (struct smb2_query_info_rsp *)iov[0].iov_base;

	if (rc) {
		cifs_stats_fail_inc(tcon, SMB2_QUERY_INFO_HE);
		goto qinf_exit;
	}

	rc = validate_and_copy_buf(le16_to_cpu(rsp->OutputBufferOffset),
				   le32_to_cpu(rsp->OutputBufferLength),
				   &rsp->hdr, min_len, data);

qinf_exit:
	free_rsp_buf(resp_buftype, rsp);
	return rc;
}

int
SMB2_query_info(const unsigned int xid, struct cifs_tcon *tcon,
		u64 persistent_fid, u64 volatile_fid,
		struct smb2_file_all_info *data)
{
	return query_info(xid, tcon, persistent_fid, volatile_fid,
			  FILE_ALL_INFORMATION,
			  sizeof(struct smb2_file_all_info) + PATH_MAX * 2,
			  sizeof(struct smb2_file_all_info), data);
}

int
SMB2_get_srv_num(const unsigned int xid, struct cifs_tcon *tcon,
		 u64 persistent_fid, u64 volatile_fid, __le64 *uniqueid)
{
	return query_info(xid, tcon, persistent_fid, volatile_fid,
			  FILE_INTERNAL_INFORMATION,
			  sizeof(struct smb2_file_internal_info),
			  sizeof(struct smb2_file_internal_info), uniqueid);
}

/*
 * This is a no-op for now. We're not really interested in the reply, but
 * rather in the fact that the server sent one and that server->lstrp
 * gets updated.
 *
 * FIXME: maybe we should consider checking that the reply matches request?
 */
static void
smb2_echo_callback(struct mid_q_entry *mid)
{
	struct TCP_Server_Info *server = mid->callback_data;
	struct smb2_echo_rsp *smb2 = (struct smb2_echo_rsp *)mid->resp_buf;
	unsigned int credits_received = 1;

	if (mid->mid_state == MID_RESPONSE_RECEIVED)
		credits_received = le16_to_cpu(smb2->hdr.CreditRequest);

	DeleteMidQEntry(mid);
	add_credits(server, credits_received, CIFS_ECHO_OP);
}

void smb2_reconnect_server(struct work_struct *work)
{
	struct TCP_Server_Info *server = container_of(work,
					struct TCP_Server_Info, reconnect.work);
	struct cifs_ses *ses;
	struct cifs_tcon *tcon, *tcon2;
	struct list_head tmp_list;
	int tcon_exist = false;

	/* Prevent simultaneous reconnects that can corrupt tcon->rlist list */
	mutex_lock(&server->reconnect_mutex);

	INIT_LIST_HEAD(&tmp_list);
	cifs_dbg(FYI, "Need negotiate, reconnecting tcons\n");

	spin_lock(&cifs_tcp_ses_lock);
	list_for_each_entry(ses, &server->smb_ses_list, smb_ses_list) {
		list_for_each_entry(tcon, &ses->tcon_list, tcon_list) {
			if (tcon->need_reconnect) {
				tcon->tc_count++;
				list_add_tail(&tcon->rlist, &tmp_list);
				tcon_exist = true;
			}
		}
	}
	/*
	 * Get the reference to server struct to be sure that the last call of
	 * cifs_put_tcon() in the loop below won't release the server pointer.
	 */
	if (tcon_exist)
		server->srv_count++;

	spin_unlock(&cifs_tcp_ses_lock);

	list_for_each_entry_safe(tcon, tcon2, &tmp_list, rlist) {
		smb2_reconnect(SMB2_ECHO, tcon);
		list_del_init(&tcon->rlist);
		cifs_put_tcon(tcon);
	}

	cifs_dbg(FYI, "Reconnecting tcons finished\n");
	mutex_unlock(&server->reconnect_mutex);

	/* now we can safely release srv struct */
	if (tcon_exist)
		cifs_put_tcp_session(server, 1);
}

int
SMB2_echo(struct TCP_Server_Info *server)
{
	struct smb2_echo_req *req;
	int rc = 0;
	struct kvec iov;
	struct smb_rqst rqst = { .rq_iov = &iov,
				 .rq_nvec = 1 };

	cifs_dbg(FYI, "In echo request\n");

	if (server->tcpStatus == CifsNeedNegotiate) {
		/* No need to send echo on newly established connections */
		queue_delayed_work(cifsiod_wq, &server->reconnect, 0);
		return rc;
	}

	rc = small_smb2_init(SMB2_ECHO, NULL, (void **)&req);
	if (rc)
		return rc;

	req->hdr.CreditRequest = cpu_to_le16(1);

	iov.iov_base = (char *)req;
	/* 4 for rfc1002 length field */
	iov.iov_len = get_rfc1002_length(req) + 4;

	rc = cifs_call_async(server, &rqst, NULL, smb2_echo_callback, server,
			     CIFS_ECHO_OP);
	if (rc)
		cifs_dbg(FYI, "Echo request failed: %d\n", rc);

	cifs_small_buf_release(req);
	return rc;
}

int
SMB2_flush(const unsigned int xid, struct cifs_tcon *tcon, u64 persistent_fid,
	   u64 volatile_fid)
{
	struct smb2_flush_req *req;
	struct TCP_Server_Info *server;
	struct cifs_ses *ses = tcon->ses;
	struct kvec iov[1];
	int resp_buftype;
	int rc = 0;

	cifs_dbg(FYI, "Flush\n");

	if (ses && (ses->server))
		server = ses->server;
	else
		return -EIO;

	rc = small_smb2_init(SMB2_FLUSH, tcon, (void **) &req);
	if (rc)
		return rc;

	req->PersistentFileId = persistent_fid;
	req->VolatileFileId = volatile_fid;

	iov[0].iov_base = (char *)req;
	/* 4 for rfc1002 length field */
	iov[0].iov_len = get_rfc1002_length(req) + 4;

	rc = SendReceive2(xid, ses, iov, 1, &resp_buftype, 0);

	if ((rc != 0) && tcon)
		cifs_stats_fail_inc(tcon, SMB2_FLUSH_HE);

	free_rsp_buf(resp_buftype, iov[0].iov_base);
	return rc;
}

/*
 * To form a chain of read requests, any read requests after the first should
 * have the end_of_chain boolean set to true.
 */
static int
smb2_new_read_req(struct kvec *iov, struct cifs_io_parms *io_parms,
		  unsigned int remaining_bytes, int request_type)
{
	int rc = -EACCES;
	struct smb2_read_req *req = NULL;

	rc = small_smb2_init(SMB2_READ, io_parms->tcon, (void **) &req);
	if (rc)
		return rc;
	if (io_parms->tcon->ses->server == NULL)
		return -ECONNABORTED;

	req->hdr.ProcessId = cpu_to_le32(io_parms->pid);

	req->PersistentFileId = io_parms->persistent_fid;
	req->VolatileFileId = io_parms->volatile_fid;
	req->ReadChannelInfoOffset = 0; /* reserved */
	req->ReadChannelInfoLength = 0; /* reserved */
	req->Channel = 0; /* reserved */
	req->MinimumCount = 0;
	req->Length = cpu_to_le32(io_parms->length);
	req->Offset = cpu_to_le64(io_parms->offset);

	if (request_type & CHAINED_REQUEST) {
		if (!(request_type & END_OF_CHAIN)) {
			/* 4 for rfc1002 length field */
			req->hdr.NextCommand =
				cpu_to_le32(get_rfc1002_length(req) + 4);
		} else /* END_OF_CHAIN */
			req->hdr.NextCommand = 0;
		if (request_type & RELATED_REQUEST) {
			req->hdr.Flags |= SMB2_FLAGS_RELATED_OPERATIONS;
			/*
			 * Related requests use info from previous read request
			 * in chain.
			 */
			req->hdr.SessionId = 0xFFFFFFFF;
			req->hdr.TreeId = 0xFFFFFFFF;
			req->PersistentFileId = 0xFFFFFFFF;
			req->VolatileFileId = 0xFFFFFFFF;
		}
	}
	if (remaining_bytes > io_parms->length)
		req->RemainingBytes = cpu_to_le32(remaining_bytes);
	else
		req->RemainingBytes = 0;

	iov[0].iov_base = (char *)req;
	/* 4 for rfc1002 length field */
	iov[0].iov_len = get_rfc1002_length(req) + 4;
	return rc;
}

static void
smb2_readv_callback(struct mid_q_entry *mid)
{
	struct cifs_readdata *rdata = mid->callback_data;
	struct cifs_tcon *tcon = tlink_tcon(rdata->cfile->tlink);
	struct TCP_Server_Info *server = tcon->ses->server;
	struct smb2_hdr *buf = (struct smb2_hdr *)rdata->iov.iov_base;
	unsigned int credits_received = 1;
	struct smb_rqst rqst = { .rq_iov = &rdata->iov,
				 .rq_nvec = 1,
				 .rq_pages = rdata->pages,
				 .rq_npages = rdata->nr_pages,
				 .rq_pagesz = rdata->pagesz,
				 .rq_tailsz = rdata->tailsz };

	cifs_dbg(FYI, "%s: mid=%llu state=%d result=%d bytes=%u\n",
		 __func__, mid->mid, mid->mid_state, rdata->result,
		 rdata->bytes);

	switch (mid->mid_state) {
	case MID_RESPONSE_RECEIVED:
		credits_received = le16_to_cpu(buf->CreditRequest);
		/* result already set, check signature */
		if (server->sec_mode &
		    (SECMODE_SIGN_REQUIRED | SECMODE_SIGN_ENABLED)) {
			int rc;

			rc = smb2_verify_signature(&rqst, server);
			if (rc)
				cifs_dbg(VFS, "SMB signature verification returned error = %d\n",
					 rc);
		}
		/* FIXME: should this be counted toward the initiating task? */
		task_io_account_read(rdata->bytes);
		cifs_stats_bytes_read(tcon, rdata->bytes);
		break;
	case MID_REQUEST_SUBMITTED:
	case MID_RETRY_NEEDED:
		rdata->result = -EAGAIN;
		break;
	default:
		if (rdata->result != -ENODATA)
			rdata->result = -EIO;
	}

	if (rdata->result)
		cifs_stats_fail_inc(tcon, SMB2_READ_HE);

	queue_work(cifsiod_wq, &rdata->work);
	DeleteMidQEntry(mid);
	add_credits(server, credits_received, 0);
}

/* smb2_async_readv - send an async write, and set up mid to handle result */
int
smb2_async_readv(struct cifs_readdata *rdata)
{
	int rc;
	struct smb2_hdr *buf;
	struct cifs_io_parms io_parms;
	struct smb_rqst rqst = { .rq_iov = &rdata->iov,
				 .rq_nvec = 1 };

	cifs_dbg(FYI, "%s: offset=%llu bytes=%u\n",
		 __func__, rdata->offset, rdata->bytes);

	io_parms.tcon = tlink_tcon(rdata->cfile->tlink);
	io_parms.offset = rdata->offset;
	io_parms.length = rdata->bytes;
	io_parms.persistent_fid = rdata->cfile->fid.persistent_fid;
	io_parms.volatile_fid = rdata->cfile->fid.volatile_fid;
	io_parms.pid = rdata->pid;
	rc = smb2_new_read_req(&rdata->iov, &io_parms, 0, 0);
	if (rc)
		return rc;

	buf = (struct smb2_hdr *)rdata->iov.iov_base;
	/* 4 for rfc1002 length field */
	rdata->iov.iov_len = get_rfc1002_length(rdata->iov.iov_base) + 4;

	kref_get(&rdata->refcount);
	rc = cifs_call_async(io_parms.tcon->ses->server, &rqst,
			     cifs_readv_receive, smb2_readv_callback,
			     rdata, 0);
	if (rc) {
		kref_put(&rdata->refcount, cifs_readdata_release);
		cifs_stats_fail_inc(io_parms.tcon, SMB2_READ_HE);
	}

	cifs_small_buf_release(buf);
	return rc;
}

int
SMB2_read(const unsigned int xid, struct cifs_io_parms *io_parms,
	  unsigned int *nbytes, char **buf, int *buf_type)
{
	int resp_buftype, rc = -EACCES;
	struct smb2_read_rsp *rsp = NULL;
	struct kvec iov[1];

	*nbytes = 0;
	rc = smb2_new_read_req(iov, io_parms, 0, 0);
	if (rc)
		return rc;

	rc = SendReceive2(xid, io_parms->tcon->ses, iov, 1,
			  &resp_buftype, CIFS_LOG_ERROR);

	rsp = (struct smb2_read_rsp *)iov[0].iov_base;

	if (rsp->hdr.Status == STATUS_END_OF_FILE) {
		free_rsp_buf(resp_buftype, iov[0].iov_base);
		return 0;
	}

	if (rc) {
		cifs_stats_fail_inc(io_parms->tcon, SMB2_READ_HE);
		cifs_dbg(VFS, "Send error in read = %d\n", rc);
	} else {
		*nbytes = le32_to_cpu(rsp->DataLength);
		if ((*nbytes > CIFS_MAX_MSGSIZE) ||
		    (*nbytes > io_parms->length)) {
			cifs_dbg(FYI, "bad length %d for count %d\n",
				 *nbytes, io_parms->length);
			rc = -EIO;
			*nbytes = 0;
		}
	}

	if (*buf) {
		memcpy(*buf, (char *)rsp->hdr.ProtocolId + rsp->DataOffset,
		       *nbytes);
		free_rsp_buf(resp_buftype, iov[0].iov_base);
	} else if (resp_buftype != CIFS_NO_BUFFER) {
		*buf = iov[0].iov_base;
		if (resp_buftype == CIFS_SMALL_BUFFER)
			*buf_type = CIFS_SMALL_BUFFER;
		else if (resp_buftype == CIFS_LARGE_BUFFER)
			*buf_type = CIFS_LARGE_BUFFER;
	}
	return rc;
}

/*
 * Check the mid_state and signature on received buffer (if any), and queue the
 * workqueue completion task.
 */
static void
smb2_writev_callback(struct mid_q_entry *mid)
{
	struct cifs_writedata *wdata = mid->callback_data;
	struct cifs_tcon *tcon = tlink_tcon(wdata->cfile->tlink);
	unsigned int written;
	struct smb2_write_rsp *rsp = (struct smb2_write_rsp *)mid->resp_buf;
	unsigned int credits_received = 1;

	switch (mid->mid_state) {
	case MID_RESPONSE_RECEIVED:
		credits_received = le16_to_cpu(rsp->hdr.CreditRequest);
		wdata->result = smb2_check_receive(mid, tcon->ses->server, 0);
		if (wdata->result != 0)
			break;

		written = le32_to_cpu(rsp->DataLength);
		/*
		 * Mask off high 16 bits when bytes written as returned
		 * by the server is greater than bytes requested by the
		 * client. OS/2 servers are known to set incorrect
		 * CountHigh values.
		 */
		if (written > wdata->bytes)
			written &= 0xFFFF;

		if (written < wdata->bytes)
			wdata->result = -ENOSPC;
		else
			wdata->bytes = written;
		break;
	case MID_REQUEST_SUBMITTED:
	case MID_RETRY_NEEDED:
		wdata->result = -EAGAIN;
		break;
	default:
		wdata->result = -EIO;
		break;
	}

	if (wdata->result)
		cifs_stats_fail_inc(tcon, SMB2_WRITE_HE);

	queue_work(cifsiod_wq, &wdata->work);
	DeleteMidQEntry(mid);
	add_credits(tcon->ses->server, credits_received, 0);
}

/* smb2_async_writev - send an async write, and set up mid to handle result */
int
smb2_async_writev(struct cifs_writedata *wdata)
{
	int rc = -EACCES;
	struct smb2_write_req *req = NULL;
	struct cifs_tcon *tcon = tlink_tcon(wdata->cfile->tlink);
	struct kvec iov;
	struct smb_rqst rqst;

	rc = small_smb2_init(SMB2_WRITE, tcon, (void **) &req);
	if (rc)
		goto async_writev_out;

	req->hdr.ProcessId = cpu_to_le32(wdata->cfile->pid);

	req->PersistentFileId = wdata->cfile->fid.persistent_fid;
	req->VolatileFileId = wdata->cfile->fid.volatile_fid;
	req->WriteChannelInfoOffset = 0;
	req->WriteChannelInfoLength = 0;
	req->Channel = 0;
	req->Offset = cpu_to_le64(wdata->offset);
	/* 4 for rfc1002 length field */
	req->DataOffset = cpu_to_le16(
				offsetof(struct smb2_write_req, Buffer) - 4);
	req->RemainingBytes = 0;

	/* 4 for rfc1002 length field and 1 for Buffer */
	iov.iov_len = get_rfc1002_length(req) + 4 - 1;
	iov.iov_base = req;

	rqst.rq_iov = &iov;
	rqst.rq_nvec = 1;
	rqst.rq_pages = wdata->pages;
	rqst.rq_npages = wdata->nr_pages;
	rqst.rq_pagesz = wdata->pagesz;
	rqst.rq_tailsz = wdata->tailsz;

	cifs_dbg(FYI, "async write at %llu %u bytes\n",
		 wdata->offset, wdata->bytes);

	req->Length = cpu_to_le32(wdata->bytes);

	inc_rfc1001_len(&req->hdr, wdata->bytes - 1 /* Buffer */);

	kref_get(&wdata->refcount);
	rc = cifs_call_async(tcon->ses->server, &rqst, NULL,
				smb2_writev_callback, wdata, 0);

	if (rc) {
		kref_put(&wdata->refcount, cifs_writedata_release);
		cifs_stats_fail_inc(tcon, SMB2_WRITE_HE);
	}

async_writev_out:
	cifs_small_buf_release(req);
	return rc;
}

/*
 * SMB2_write function gets iov pointer to kvec array with n_vec as a length.
 * The length field from io_parms must be at least 1 and indicates a number of
 * elements with data to write that begins with position 1 in iov array. All
 * data length is specified by count.
 */
int
SMB2_write(const unsigned int xid, struct cifs_io_parms *io_parms,
	   unsigned int *nbytes, struct kvec *iov, int n_vec)
{
	int rc = 0;
	struct smb2_write_req *req = NULL;
	struct smb2_write_rsp *rsp = NULL;
	int resp_buftype;
	*nbytes = 0;

	if (n_vec < 1)
		return rc;

	rc = small_smb2_init(SMB2_WRITE, io_parms->tcon, (void **) &req);
	if (rc)
		return rc;

	if (io_parms->tcon->ses->server == NULL)
		return -ECONNABORTED;

	req->hdr.ProcessId = cpu_to_le32(io_parms->pid);

	req->PersistentFileId = io_parms->persistent_fid;
	req->VolatileFileId = io_parms->volatile_fid;
	req->WriteChannelInfoOffset = 0;
	req->WriteChannelInfoLength = 0;
	req->Channel = 0;
	req->Length = cpu_to_le32(io_parms->length);
	req->Offset = cpu_to_le64(io_parms->offset);
	/* 4 for rfc1002 length field */
	req->DataOffset = cpu_to_le16(
				offsetof(struct smb2_write_req, Buffer) - 4);
	req->RemainingBytes = 0;

	iov[0].iov_base = (char *)req;
	/* 4 for rfc1002 length field and 1 for Buffer */
	iov[0].iov_len = get_rfc1002_length(req) + 4 - 1;

	/* length of entire message including data to be written */
	inc_rfc1001_len(req, io_parms->length - 1 /* Buffer */);

	rc = SendReceive2(xid, io_parms->tcon->ses, iov, n_vec + 1,
			  &resp_buftype, 0);
	rsp = (struct smb2_write_rsp *)iov[0].iov_base;

	if (rc) {
		cifs_stats_fail_inc(io_parms->tcon, SMB2_WRITE_HE);
		cifs_dbg(VFS, "Send error in write = %d\n", rc);
	} else
		*nbytes = le32_to_cpu(rsp->DataLength);

	free_rsp_buf(resp_buftype, rsp);
	return rc;
}

static unsigned int
num_entries(char *bufstart, char *end_of_buf, char **lastentry, size_t size)
{
	int len;
	unsigned int entrycount = 0;
	unsigned int next_offset = 0;
	FILE_DIRECTORY_INFO *entryptr;

	if (bufstart == NULL)
		return 0;

	entryptr = (FILE_DIRECTORY_INFO *)bufstart;

	while (1) {
		entryptr = (FILE_DIRECTORY_INFO *)
					((char *)entryptr + next_offset);

		if ((char *)entryptr + size > end_of_buf) {
			cifs_dbg(VFS, "malformed search entry would overflow\n");
			break;
		}

		len = le32_to_cpu(entryptr->FileNameLength);
		if ((char *)entryptr + len + size > end_of_buf) {
			cifs_dbg(VFS, "directory entry name would overflow frame end of buf %p\n",
				 end_of_buf);
			break;
		}

		*lastentry = (char *)entryptr;
		entrycount++;

		next_offset = le32_to_cpu(entryptr->NextEntryOffset);
		if (!next_offset)
			break;
	}

	return entrycount;
}

/*
 * Readdir/FindFirst
 */
int
SMB2_query_directory(const unsigned int xid, struct cifs_tcon *tcon,
		     u64 persistent_fid, u64 volatile_fid, int index,
		     struct cifs_search_info *srch_inf)
{
	struct smb2_query_directory_req *req;
	struct smb2_query_directory_rsp *rsp = NULL;
	struct kvec iov[2];
	int rc = 0;
	int len;
	int resp_buftype;
	unsigned char *bufptr;
	struct TCP_Server_Info *server;
	struct cifs_ses *ses = tcon->ses;
	__le16 asteriks = cpu_to_le16('*');
	char *end_of_smb;
	unsigned int output_size = CIFSMaxBufSize;
	size_t info_buf_size;

	if (ses && (ses->server))
		server = ses->server;
	else
		return -EIO;

	rc = small_smb2_init(SMB2_QUERY_DIRECTORY, tcon, (void **) &req);
	if (rc)
		return rc;

	switch (srch_inf->info_level) {
	case SMB_FIND_FILE_DIRECTORY_INFO:
		req->FileInformationClass = FILE_DIRECTORY_INFORMATION;
		info_buf_size = sizeof(FILE_DIRECTORY_INFO) - 1;
		break;
	case SMB_FIND_FILE_ID_FULL_DIR_INFO:
		req->FileInformationClass = FILEID_FULL_DIRECTORY_INFORMATION;
		info_buf_size = sizeof(SEARCH_ID_FULL_DIR_INFO) - 1;
		break;
	default:
		cifs_dbg(VFS, "info level %u isn't supported\n",
			 srch_inf->info_level);
		rc = -EINVAL;
		goto qdir_exit;
	}

	req->FileIndex = cpu_to_le32(index);
	req->PersistentFileId = persistent_fid;
	req->VolatileFileId = volatile_fid;

	len = 0x2;
	bufptr = req->Buffer;
	memcpy(bufptr, &asteriks, len);

	req->FileNameOffset =
		cpu_to_le16(sizeof(struct smb2_query_directory_req) - 1 - 4);
	req->FileNameLength = cpu_to_le16(len);
	/*
	 * BB could be 30 bytes or so longer if we used SMB2 specific
	 * buffer lengths, but this is safe and close enough.
	 */
	output_size = min_t(unsigned int, output_size, server->maxBuf);
	output_size = min_t(unsigned int, output_size, 2 << 15);
	req->OutputBufferLength = cpu_to_le32(output_size);

	iov[0].iov_base = (char *)req;
	/* 4 for RFC1001 length and 1 for Buffer */
	iov[0].iov_len = get_rfc1002_length(req) + 4 - 1;

	iov[1].iov_base = (char *)(req->Buffer);
	iov[1].iov_len = len;

	inc_rfc1001_len(req, len - 1 /* Buffer */);

	rc = SendReceive2(xid, ses, iov, 2, &resp_buftype, 0);
	rsp = (struct smb2_query_directory_rsp *)iov[0].iov_base;

	if (rc) {
		if (rc == -ENODATA && rsp->hdr.Status == STATUS_NO_MORE_FILES) {
			srch_inf->endOfSearch = true;
			rc = 0;
		}
		cifs_stats_fail_inc(tcon, SMB2_QUERY_DIRECTORY_HE);
		goto qdir_exit;
	}

	rc = validate_buf(le16_to_cpu(rsp->OutputBufferOffset),
			  le32_to_cpu(rsp->OutputBufferLength), &rsp->hdr,
			  info_buf_size);
	if (rc)
		goto qdir_exit;

	srch_inf->unicode = true;

	if (srch_inf->ntwrk_buf_start) {
		if (srch_inf->smallBuf)
			cifs_small_buf_release(srch_inf->ntwrk_buf_start);
		else
			cifs_buf_release(srch_inf->ntwrk_buf_start);
	}
	srch_inf->ntwrk_buf_start = (char *)rsp;
	srch_inf->srch_entries_start = srch_inf->last_entry = 4 /* rfclen */ +
		(char *)&rsp->hdr + le16_to_cpu(rsp->OutputBufferOffset);
	/* 4 for rfc1002 length field */
	end_of_smb = get_rfc1002_length(rsp) + 4 + (char *)&rsp->hdr;
	srch_inf->entries_in_buffer =
			num_entries(srch_inf->srch_entries_start, end_of_smb,
				    &srch_inf->last_entry, info_buf_size);
	srch_inf->index_of_last_entry += srch_inf->entries_in_buffer;
	cifs_dbg(FYI, "num entries %d last_index %lld srch start %p srch end %p\n",
		 srch_inf->entries_in_buffer, srch_inf->index_of_last_entry,
		 srch_inf->srch_entries_start, srch_inf->last_entry);
	if (resp_buftype == CIFS_LARGE_BUFFER)
		srch_inf->smallBuf = false;
	else if (resp_buftype == CIFS_SMALL_BUFFER)
		srch_inf->smallBuf = true;
	else
		cifs_dbg(VFS, "illegal search buffer type\n");

	return rc;

qdir_exit:
	free_rsp_buf(resp_buftype, rsp);
	return rc;
}

static int
send_set_info(const unsigned int xid, struct cifs_tcon *tcon,
	       u64 persistent_fid, u64 volatile_fid, u32 pid, int info_class,
	       unsigned int num, void **data, unsigned int *size)
{
	struct smb2_set_info_req *req;
	struct smb2_set_info_rsp *rsp = NULL;
	struct kvec *iov;
	int rc = 0;
	int resp_buftype;
	unsigned int i;
	struct TCP_Server_Info *server;
	struct cifs_ses *ses = tcon->ses;

	if (ses && (ses->server))
		server = ses->server;
	else
		return -EIO;

	if (!num)
		return -EINVAL;

	iov = kmalloc(sizeof(struct kvec) * num, GFP_KERNEL);
	if (!iov)
		return -ENOMEM;

	rc = small_smb2_init(SMB2_SET_INFO, tcon, (void **) &req);
	if (rc) {
		kfree(iov);
		return rc;
	}

	req->hdr.ProcessId = cpu_to_le32(pid);

	req->InfoType = SMB2_O_INFO_FILE;
	req->FileInfoClass = info_class;
	req->PersistentFileId = persistent_fid;
	req->VolatileFileId = volatile_fid;

	/* 4 for RFC1001 length and 1 for Buffer */
	req->BufferOffset =
			cpu_to_le16(sizeof(struct smb2_set_info_req) - 1 - 4);
	req->BufferLength = cpu_to_le32(*size);

	inc_rfc1001_len(req, *size - 1 /* Buffer */);

	memcpy(req->Buffer, *data, *size);

	iov[0].iov_base = (char *)req;
	/* 4 for RFC1001 length */
	iov[0].iov_len = get_rfc1002_length(req) + 4;

	for (i = 1; i < num; i++) {
		inc_rfc1001_len(req, size[i]);
		le32_add_cpu(&req->BufferLength, size[i]);
		iov[i].iov_base = (char *)data[i];
		iov[i].iov_len = size[i];
	}

	rc = SendReceive2(xid, ses, iov, num, &resp_buftype, 0);
	rsp = (struct smb2_set_info_rsp *)iov[0].iov_base;

	if (rc != 0) {
		cifs_stats_fail_inc(tcon, SMB2_SET_INFO_HE);
		goto out;
	}
out:
	free_rsp_buf(resp_buftype, rsp);
	kfree(iov);
	return rc;
}

int
SMB2_rename(const unsigned int xid, struct cifs_tcon *tcon,
	    u64 persistent_fid, u64 volatile_fid, __le16 *target_file)
{
	struct smb2_file_rename_info info;
	void **data;
	unsigned int size[2];
	int rc;
	int len = (2 * UniStrnlen((wchar_t *)target_file, PATH_MAX));

	data = kmalloc(sizeof(void *) * 2, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	info.ReplaceIfExists = 1; /* 1 = replace existing target with new */
			      /* 0 = fail if target already exists */
	info.RootDirectory = 0;  /* MBZ for network ops (why does spec say?) */
	info.FileNameLength = cpu_to_le32(len);

	data[0] = &info;
	size[0] = sizeof(struct smb2_file_rename_info);

	data[1] = target_file;
	size[1] = len + 2 /* null */;

	rc = send_set_info(xid, tcon, persistent_fid, volatile_fid,
			   current->tgid, FILE_RENAME_INFORMATION, 2, data,
			   size);
	kfree(data);
	return rc;
}

int
SMB2_set_hardlink(const unsigned int xid, struct cifs_tcon *tcon,
		  u64 persistent_fid, u64 volatile_fid, __le16 *target_file)
{
	struct smb2_file_link_info info;
	void **data;
	unsigned int size[2];
	int rc;
	int len = (2 * UniStrnlen((wchar_t *)target_file, PATH_MAX));

	data = kmalloc(sizeof(void *) * 2, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	info.ReplaceIfExists = 0; /* 1 = replace existing link with new */
			      /* 0 = fail if link already exists */
	info.RootDirectory = 0;  /* MBZ for network ops (why does spec say?) */
	info.FileNameLength = cpu_to_le32(len);

	data[0] = &info;
	size[0] = sizeof(struct smb2_file_link_info);

	data[1] = target_file;
	size[1] = len + 2 /* null */;

	rc = send_set_info(xid, tcon, persistent_fid, volatile_fid,
			   current->tgid, FILE_LINK_INFORMATION, 2, data, size);
	kfree(data);
	return rc;
}

int
SMB2_set_eof(const unsigned int xid, struct cifs_tcon *tcon, u64 persistent_fid,
	     u64 volatile_fid, u32 pid, __le64 *eof)
{
	struct smb2_file_eof_info info;
	void *data;
	unsigned int size;

	info.EndOfFile = *eof;

	data = &info;
	size = sizeof(struct smb2_file_eof_info);

	return send_set_info(xid, tcon, persistent_fid, volatile_fid, pid,
			     FILE_END_OF_FILE_INFORMATION, 1, &data, &size);
}

int
SMB2_set_info(const unsigned int xid, struct cifs_tcon *tcon,
	      u64 persistent_fid, u64 volatile_fid, FILE_BASIC_INFO *buf)
{
	unsigned int size;
	size = sizeof(FILE_BASIC_INFO);
	return send_set_info(xid, tcon, persistent_fid, volatile_fid,
			     current->tgid, FILE_BASIC_INFORMATION, 1,
			     (void **)&buf, &size);
}

int
SMB2_oplock_break(const unsigned int xid, struct cifs_tcon *tcon,
		  const u64 persistent_fid, const u64 volatile_fid,
		  __u8 oplock_level)
{
	int rc;
	struct smb2_oplock_break *req = NULL;

	cifs_dbg(FYI, "SMB2_oplock_break\n");
	rc = small_smb2_init(SMB2_OPLOCK_BREAK, tcon, (void **) &req);

	if (rc)
		return rc;

	req->VolatileFid = volatile_fid;
	req->PersistentFid = persistent_fid;
	req->OplockLevel = oplock_level;
	req->hdr.CreditRequest = cpu_to_le16(1);

	rc = SendReceiveNoRsp(xid, tcon->ses, (char *) req, CIFS_OBREAK_OP);
	/* SMB2 buffer freed by function above */

	if (rc) {
		cifs_stats_fail_inc(tcon, SMB2_OPLOCK_BREAK_HE);
		cifs_dbg(FYI, "Send error in Oplock Break = %d\n", rc);
	}

	return rc;
}

static void
copy_fs_info_to_kstatfs(struct smb2_fs_full_size_info *pfs_inf,
			struct kstatfs *kst)
{
	kst->f_bsize = le32_to_cpu(pfs_inf->BytesPerSector) *
			  le32_to_cpu(pfs_inf->SectorsPerAllocationUnit);
	kst->f_blocks = le64_to_cpu(pfs_inf->TotalAllocationUnits);
	kst->f_bfree  = le64_to_cpu(pfs_inf->ActualAvailableAllocationUnits);
	kst->f_bavail = le64_to_cpu(pfs_inf->CallerAvailableAllocationUnits);
	return;
}

static int
build_qfs_info_req(struct kvec *iov, struct cifs_tcon *tcon, int level,
		   int outbuf_len, u64 persistent_fid, u64 volatile_fid)
{
	int rc;
	struct smb2_query_info_req *req;

	cifs_dbg(FYI, "Query FSInfo level %d\n", level);

	if ((tcon->ses == NULL) || (tcon->ses->server == NULL))
		return -EIO;

	rc = small_smb2_init(SMB2_QUERY_INFO, tcon, (void **) &req);
	if (rc)
		return rc;

	req->InfoType = SMB2_O_INFO_FILESYSTEM;
	req->FileInfoClass = level;
	req->PersistentFileId = persistent_fid;
	req->VolatileFileId = volatile_fid;
	/* 4 for rfc1002 length field and 1 for pad */
	req->InputBufferOffset =
			cpu_to_le16(sizeof(struct smb2_query_info_req) - 1 - 4);
	req->OutputBufferLength = cpu_to_le32(
		outbuf_len + sizeof(struct smb2_query_info_rsp) - 1 - 4);

	iov->iov_base = (char *)req;
	/* 4 for rfc1002 length field */
	iov->iov_len = get_rfc1002_length(req) + 4;
	return 0;
}

int
SMB2_QFS_info(const unsigned int xid, struct cifs_tcon *tcon,
	      u64 persistent_fid, u64 volatile_fid, struct kstatfs *fsdata)
{
	struct smb2_query_info_rsp *rsp = NULL;
	struct kvec iov;
	int rc = 0;
	int resp_buftype;
	struct cifs_ses *ses = tcon->ses;
	struct smb2_fs_full_size_info *info = NULL;

	rc = build_qfs_info_req(&iov, tcon, FS_FULL_SIZE_INFORMATION,
				sizeof(struct smb2_fs_full_size_info),
				persistent_fid, volatile_fid);
	if (rc)
		return rc;

	rc = SendReceive2(xid, ses, &iov, 1, &resp_buftype, 0);
	if (rc) {
		cifs_stats_fail_inc(tcon, SMB2_QUERY_INFO_HE);
		goto qinf_exit;
	}
	rsp = (struct smb2_query_info_rsp *)iov.iov_base;

	info = (struct smb2_fs_full_size_info *)(4 /* RFC1001 len */ +
		le16_to_cpu(rsp->OutputBufferOffset) + (char *)&rsp->hdr);
	rc = validate_buf(le16_to_cpu(rsp->OutputBufferOffset),
			  le32_to_cpu(rsp->OutputBufferLength), &rsp->hdr,
			  sizeof(struct smb2_fs_full_size_info));
	if (!rc)
		copy_fs_info_to_kstatfs(info, fsdata);

qinf_exit:
	free_rsp_buf(resp_buftype, iov.iov_base);
	return rc;
}

int
smb2_lockv(const unsigned int xid, struct cifs_tcon *tcon,
	   const __u64 persist_fid, const __u64 volatile_fid, const __u32 pid,
	   const __u32 num_lock, struct smb2_lock_element *buf)
{
	int rc = 0;
	struct smb2_lock_req *req = NULL;
	struct kvec iov[2];
	int resp_buf_type;
	unsigned int count;

	cifs_dbg(FYI, "smb2_lockv num lock %d\n", num_lock);

	rc = small_smb2_init(SMB2_LOCK, tcon, (void **) &req);
	if (rc)
		return rc;

	req->hdr.ProcessId = cpu_to_le32(pid);
	req->LockCount = cpu_to_le16(num_lock);

	req->PersistentFileId = persist_fid;
	req->VolatileFileId = volatile_fid;

	count = num_lock * sizeof(struct smb2_lock_element);
	inc_rfc1001_len(req, count - sizeof(struct smb2_lock_element));

	iov[0].iov_base = (char *)req;
	/* 4 for rfc1002 length field and count for all locks */
	iov[0].iov_len = get_rfc1002_length(req) + 4 - count;
	iov[1].iov_base = (char *)buf;
	iov[1].iov_len = count;

	cifs_stats_inc(&tcon->stats.cifs_stats.num_locks);
	rc = SendReceive2(xid, tcon->ses, iov, 2, &resp_buf_type, CIFS_NO_RESP);
	if (rc) {
		cifs_dbg(FYI, "Send error in smb2_lockv = %d\n", rc);
		cifs_stats_fail_inc(tcon, SMB2_LOCK_HE);
	}

	return rc;
}

int
SMB2_lock(const unsigned int xid, struct cifs_tcon *tcon,
	  const __u64 persist_fid, const __u64 volatile_fid, const __u32 pid,
	  const __u64 length, const __u64 offset, const __u32 lock_flags,
	  const bool wait)
{
	struct smb2_lock_element lock;

	lock.Offset = cpu_to_le64(offset);
	lock.Length = cpu_to_le64(length);
	lock.Flags = cpu_to_le32(lock_flags);
	if (!wait && lock_flags != SMB2_LOCKFLAG_UNLOCK)
		lock.Flags |= cpu_to_le32(SMB2_LOCKFLAG_FAIL_IMMEDIATELY);

	return smb2_lockv(xid, tcon, persist_fid, volatile_fid, pid, 1, &lock);
}

int
SMB2_lease_break(const unsigned int xid, struct cifs_tcon *tcon,
		 __u8 *lease_key, const __le32 lease_state)
{
	int rc;
	struct smb2_lease_ack *req = NULL;

	cifs_dbg(FYI, "SMB2_lease_break\n");
	rc = small_smb2_init(SMB2_OPLOCK_BREAK, tcon, (void **) &req);

	if (rc)
		return rc;

	req->hdr.CreditRequest = cpu_to_le16(1);
	req->StructureSize = cpu_to_le16(36);
	inc_rfc1001_len(req, 12);

	memcpy(req->LeaseKey, lease_key, 16);
	req->LeaseState = lease_state;

	rc = SendReceiveNoRsp(xid, tcon->ses, (char *) req, CIFS_OBREAK_OP);
	/* SMB2 buffer freed by function above */

	if (rc) {
		cifs_stats_fail_inc(tcon, SMB2_OPLOCK_BREAK_HE);
		cifs_dbg(FYI, "Send error in Lease Break = %d\n", rc);
	}

	return rc;
}
