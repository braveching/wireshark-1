/* packet-afp.c
 * Routines for afp packet dissection
 * Copyright 2002, Didier Gautheron <dgautheron@magic.fr>
 *
 * $Id: packet-afp.c,v 1.8 2002/04/28 22:16:50 guy Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@ethereal.com>
 * Copyright 1998 Gerald Combs
 *
 * Copied from README.developer
 * Copied from packet-dsi.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif

#ifdef NEED_SNPRINTF_H
# ifdef HAVE_STDARG_H
#  include <stdarg.h>
# else
#  include <varargs.h>
# endif
# include "snprintf.h"
#endif

#include <string.h>
#include <glib.h>
#include <epan/packet.h>
#include <epan/strutil.h>
#include <epan/conversation.h>

#include "packet-afp.h"

/* The information in this module (AFP) comes from:

  AFP 2.1 & 2.2.pdf contained in AppleShare_IP_6.3_SDK
  available from http://www.apple.com
 
  AFP3.0.pdf from http://www.apple.com
  
  The netatalk source code by Wesley Craig & Adrian Sun
	http://netatalk.sf.net
*/
/* from netatalk/include/afp.h */
#define AFPTRANS_NONE          0
#define AFPTRANS_DDP          (1 << 0)
#define AFPTRANS_TCP          (1 << 1)
#define AFPTRANS_ALL          (AFPTRANS_DDP | AFPTRANS_TCP)

/* server flags */
#define AFPSRVRINFO_COPY	 		(1<<0)  /* supports copyfile */
#define AFPSRVRINFO_PASSWD	 		(1<<1)	/* supports change password */
#define AFPSRVRINFO_NOSAVEPASSWD 	(1<<2)  /* don't allow save password */
#define AFPSRVRINFO_SRVMSGS      	(1<<3)  /* supports server messages */
#define AFPSRVRINFO_SRVSIGNATURE 	(1<<4)  /* supports server signature */
#define AFPSRVRINFO_TCPIP        	(1<<5)  /* supports tcpip */
#define AFPSRVRINFO_SRVNOTIFY    	(1<<6)  /* supports server notifications */ 
#define AFPSRVRINFO_FASTBOZO	 	(1<<15) /* fast copying */

/* AFP Attention Codes -- 4 bits */
#define AFPATTN_SHUTDOWN     (1 << 15)            /* shutdown/disconnect */
#define AFPATTN_CRASH        (1 << 14)            /* server crashed */
#define AFPATTN_MESG         (1 << 13)            /* server has message */
#define AFPATTN_NORECONNECT  (1 << 12)            /* don't reconnect */
/* server notification */
#define AFPATTN_NOTIFY       (AFPATTN_MESG | AFPATTN_NORECONNECT) 

/* extended bitmap -- 12 bits. volchanged is only useful w/ a server
 * notification, and time is only useful for shutdown. */
#define AFPATTN_VOLCHANGED   (1 << 0)             /* volume has changed */
#define AFPATTN_TIME(x)      ((x) & 0xfff)        /* time in minutes */

/* AFP functions */
#define AFP_BYTELOCK	     1
#define AFP_CLOSEVOL     	 2
#define AFP_CLOSEDIR     	 3
#define AFP_CLOSEFORK	 	 4
#define AFP_COPYFILE 	 	 5
#define AFP_CREATEDIR		 6
#define AFP_CREATEFILE		 7
#define AFP_DELETE	 	     8
#define AFP_ENUMERATE	 	 9
#define AFP_FLUSH		    10
#define AFP_FLUSHFORK		11
#define AFP_GETFORKPARAM	14
#define AFP_GETSRVINFO  	15
#define AFP_GETSRVPARAM 	16
#define AFP_GETVOLPARAM		17
#define AFP_LOGIN       	18
#define AFP_LOGINCONT		19
#define AFP_LOGOUT      	20
#define AFP_MAPID		    21
#define AFP_MAPNAME		    22
#define AFP_MOVE		    23
#define AFP_OPENVOL     	24
#define AFP_OPENDIR		    25
#define AFP_OPENFORK		26
#define AFP_READ		    27
#define AFP_RENAME		    28
#define AFP_SETDIRPARAM		29
#define AFP_SETFILEPARAM	30
#define AFP_SETFORKPARAM	31
#define AFP_SETVOLPARAM		32
#define AFP_WRITE		    33
#define AFP_GETFLDRPARAM	34
#define AFP_SETFLDRPARAM	35
#define AFP_CHANGEPW    	36
#define AFP_GETSRVRMSG		38
#define AFP_CREATEID		39
#define AFP_DELETEID		40
#define AFP_RESOLVEID		41
#define AFP_EXCHANGEFILE	42
#define AFP_CATSEARCH		43
#define AFP_OPENDT		    48
#define AFP_CLOSEDT		    49
#define AFP_GETICON         51
#define AFP_GTICNINFO       52
#define AFP_ADDAPPL         53
#define AFP_RMVAPPL         54
#define AFP_GETAPPL         55
#define AFP_ADDCMT          56
#define AFP_RMVCMT          57
#define AFP_GETCMT          58
#define AFP_ADDICON        192

/* ----------------------------- */
static int proto_afp = -1;
static int hf_afp_requestid = -1;
static int hf_afp_code = -1;
static int hf_afp_length = -1;
static int hf_afp_reserved = -1;

static int hf_afp_command = -1;		/* CommandCode */
static int hf_afp_AFPVersion = -1; 
static int hf_afp_UAM = -1; 
static int hf_afp_user = -1; 
static int hf_afp_passwd = -1; 
static int hf_afp_pad = -1;

static int hf_afp_vol_bitmap = -1;
static int hf_afp_bitmap_offset = -1;
static int hf_afp_vol_id = -1;
static int hf_afp_vol_attribute = -1;
static int hf_afp_vol_name = -1;
static int hf_afp_vol_signature = -1;
static int hf_afp_vol_creation_date = -1;
static int hf_afp_vol_modification_date = -1;
static int hf_afp_vol_backup_date = -1;
static int hf_afp_vol_bytes_free = -1;
static int hf_afp_vol_bytes_total = -1;
static int hf_afp_vol_ex_bytes_free = -1;
static int hf_afp_vol_ex_bytes_total = -1;
static int hf_afp_vol_block_size = -1;

/* desktop stuff */
static int hf_afp_comment 		= -1;
static int hf_afp_file_creator 	= -1;
static int hf_afp_file_type 	= -1;
static int hf_afp_icon_type 	= -1;
static int hf_afp_icon_length 	= -1;
static int hf_afp_icon_tag		= -1;
static int hf_afp_icon_index	= -1;
static int hf_afp_appl_index	= -1;
static int hf_afp_appl_tag		= -1;

static int hf_afp_did 				  = -1;
static int hf_afp_file_id 			  = -1;
static int hf_afp_file_DataForkLen    = -1;
static int hf_afp_file_RsrcForkLen    = -1;
static int hf_afp_file_ExtDataForkLen = -1;
static int hf_afp_file_ExtRsrcForkLen = -1;
static int hf_afp_file_UnixPrivs      = -1;

static int hf_afp_dir_bitmap 	 = -1;
static int hf_afp_dir_offspring  = -1;
static int hf_afp_dir_OwnerID    = -1;
static int hf_afp_dir_GroupID    = -1;

static int hf_afp_file_bitmap = -1;
static int hf_afp_req_count = -1;
static int hf_afp_start_index = -1;
static int hf_afp_max_reply_size = -1;
static int hf_afp_file_flag = -1;
static int hf_afp_create_flag = -1;
static int hf_afp_struct_size = -1;

static int hf_afp_creation_date = -1;
static int hf_afp_modification_date = -1;
static int hf_afp_backup_date = -1;
static int hf_afp_finder_info = -1;

static int hf_afp_path_type = -1;
static int hf_afp_path_len = -1;
static int hf_afp_path_name = -1;

static int hf_afp_flag		= -1;
static int hf_afp_dt_ref	= -1;
static int hf_afp_ofork		= -1;
static int hf_afp_ofork_len	= -1;
static int hf_afp_offset	= -1;
static int hf_afp_rw_count	= -1;
static int hf_afp_last_written	= -1;
static int hf_afp_actual_count	= -1;

static int hf_afp_fork_type			= -1;
static int hf_afp_access_mode		= -1;
static int hf_afp_access_read		= -1;
static int hf_afp_access_write		= -1;
static int hf_afp_access_deny_read  = -1;
static int hf_afp_access_deny_write = -1;

static gint hf_afp_lock_op			= -1;
static gint hf_afp_lock_from		= -1;
static gint hf_afp_lock_offset  	= -1;
static gint hf_afp_lock_len     	= -1;
static gint hf_afp_lock_range_start = -1;

static gint ett_afp = -1;

static gint ett_afp_vol_attribute = -1;
static gint ett_afp_enumerate = -1;
static gint ett_afp_enumerate_line = -1;
static gint ett_afp_access_mode = -1;

static gint ett_afp_vol_bitmap = -1;
static gint ett_afp_dir_bitmap = -1;
static gint ett_afp_dir_attribute = -1;
static gint ett_afp_file_attribute = -1;
static gint ett_afp_file_bitmap = -1;
static gint ett_afp_path_name = -1;
static gint ett_afp_lock_flags = -1;
static gint ett_afp_dir_ar = -1;

static dissector_handle_t afp_handle;
static dissector_handle_t data_handle;

static const value_string vol_signature_vals[] = {
	{1, "Flat"},
	{2, "Fixed Directory ID"},
	{3, "Variable Directory ID (deprecated)"},
	{0, NULL }
};

static const value_string CommandCode_vals[] = {
  {AFP_BYTELOCK,	"FPByteRangeLock" },
  {AFP_CLOSEVOL,	"FPCloseVol" },
  {AFP_CLOSEDIR,	"FPCloseDir" },
  {AFP_CLOSEFORK,	"FPCloseFork" },
  {AFP_COPYFILE,	"FPCopyFile" },
  {AFP_CREATEDIR,	"FPCreateDir" },
  {AFP_CREATEFILE,	"FPCreateFile" },
  {AFP_DELETE,		"FPDelete" },
  {AFP_ENUMERATE,	"FPEnumerate" },
  {AFP_FLUSH,		"FPFlush" },
  {AFP_FLUSHFORK,	"FPFlushFork" },
  {AFP_GETFORKPARAM,	"FPGetForkParms" },
  {AFP_GETSRVINFO,	"FPGetSrvrInfo" },
  {AFP_GETSRVPARAM,	"FPGetSrvrParms" },
  {AFP_GETVOLPARAM,	"FPGetVolParms" },
  {AFP_LOGIN,		"FPLogin" },
  {AFP_LOGINCONT,	"FPLoginCont" },
  {AFP_LOGOUT,		"FPLogout" },
  {AFP_MAPID,		"FPMapID" },
  {AFP_MAPNAME,		"FPMapName" },
  {AFP_MOVE,		"FPMoveAndRename" },
  {AFP_OPENVOL,		"FPOpenVol" },
  {AFP_OPENDIR,		"FPOpenDir" },
  {AFP_OPENFORK,	"FPOpenFork" },
  {AFP_READ,		"FPRead" },
  {AFP_RENAME,		"FPRename" },
  {AFP_SETDIRPARAM,	"FPSetDirParms" },
  {AFP_SETFILEPARAM,	"FPSetFileParms" },
  {AFP_SETFORKPARAM,	"FPSetForkParms" },
  {AFP_SETVOLPARAM,	"FPSetVolParms" },
  {AFP_WRITE,		"FPWrite" },
  {AFP_GETFLDRPARAM,	"FPGetFileDirParms" },
  {AFP_SETFLDRPARAM,	"FPSetFileDirParms" },
  {AFP_CHANGEPW,	"FPChangePassword" },
  {AFP_GETSRVRMSG,	"FPGetSrvrMsg" },
  {AFP_CREATEID,	"FPCreateID" },
  {AFP_DELETEID,	"FPDeleteID" },
  {AFP_RESOLVEID,	"FPResolveID" },
  {AFP_EXCHANGEFILE,	"FPExchangeFiles" },
  {AFP_CATSEARCH,	"FPCatSearch" },
  {AFP_OPENDT,		"FPOpenDT" },
  {AFP_CLOSEDT,		"FPCloseDT" },
  {AFP_GETICON,		"FPGetIcon" },
  {AFP_GTICNINFO,	"FPGetIconInfo" },
  {AFP_ADDAPPL,		"FPAddAPPL" },
  {AFP_RMVAPPL,		"FPRemoveAPPL" },
  {AFP_GETAPPL,		"FPGetAPPL" },
  {AFP_ADDCMT,		"FPAddComment" },
  {AFP_RMVCMT,		"FPRemoveComment" },
  {AFP_GETCMT,		"FPGetComment" },
  {AFP_ADDICON,		"FPAddIcon" },
  {0,			 NULL }
};


/* volume bitmap
  from Apple AFP3.0.pdf 
  Table 1-2 p. 20
*/
#define kFPVolAttributeBit 		(1 << 0)
#define kFPVolSignatureBit 		(1 << 1)
#define kFPVolCreateDateBit	 	(1 << 2)
#define kFPVolModDateBit 		(1 << 3)
#define kFPVolBackupDateBit 		(1 << 4)
#define kFPVolIDBit 			(1 << 5)
#define kFPVolBytesFreeBit	  	(1 << 6)
#define kFPVolBytesTotalBit	 	(1 << 7)
#define kFPVolNameBit 			(1 << 8)
#define kFPVolExtBytesFreeBit 		(1 << 9)
#define kFPVolExtBytesTotalBit		(1 << 10)
#define kFPVolBlockSizeBit 	  	(1 << 11)

static int hf_afp_vol_bitmap_Attributes 	= -1;
static int hf_afp_vol_bitmap_Signature 		= -1;
static int hf_afp_vol_bitmap_CreateDate 	= -1;
static int hf_afp_vol_bitmap_ModDate 		= -1;
static int hf_afp_vol_bitmap_BackupDate 	= -1;
static int hf_afp_vol_bitmap_ID 		= -1;
static int hf_afp_vol_bitmap_BytesFree 		= -1;
static int hf_afp_vol_bitmap_BytesTotal 	= -1;
static int hf_afp_vol_bitmap_Name 		= -1;
static int hf_afp_vol_bitmap_ExtBytesFree 	= -1;
static int hf_afp_vol_bitmap_ExtBytesTotal 	= -1;
static int hf_afp_vol_bitmap_BlockSize 		= -1;

static int hf_afp_vol_attribute_ReadOnly			= -1;
static int hf_afp_vol_attribute_HasVolumePassword		= -1;
static int hf_afp_vol_attribute_SupportsFileIDs			= -1;
static int hf_afp_vol_attribute_SupportsCatSearch		= -1;
static int hf_afp_vol_attribute_SupportsBlankAccessPrivs	= -1;
static int hf_afp_vol_attribute_SupportsUnixPrivs		= -1;
static int hf_afp_vol_attribute_SupportsUTF8Names		= -1;

static int hf_afp_dir_bitmap_Attributes     = -1;
static int hf_afp_dir_bitmap_ParentDirID    = -1;
static int hf_afp_dir_bitmap_CreateDate     = -1;
static int hf_afp_dir_bitmap_ModDate        = -1;
static int hf_afp_dir_bitmap_BackupDate     = -1;
static int hf_afp_dir_bitmap_FinderInfo     = -1;
static int hf_afp_dir_bitmap_LongName       = -1;
static int hf_afp_dir_bitmap_ShortName      = -1;
static int hf_afp_dir_bitmap_NodeID         = -1;
static int hf_afp_dir_bitmap_OffspringCount = -1;
static int hf_afp_dir_bitmap_OwnerID        = -1;
static int hf_afp_dir_bitmap_GroupID        = -1;
static int hf_afp_dir_bitmap_AccessRights   = -1;
static int hf_afp_dir_bitmap_UTF8Name       = -1;
static int hf_afp_dir_bitmap_UnixPrivs      = -1;

static int hf_afp_dir_attribute_Invisible     = -1;
static int hf_afp_dir_attribute_IsExpFolder   = -1;

static int hf_afp_dir_attribute_System        = -1;
static int hf_afp_dir_attribute_Mounted       = -1;
static int hf_afp_dir_attribute_InExpFolder   = -1;

static int hf_afp_dir_attribute_BackUpNeeded  = -1;
static int hf_afp_dir_attribute_RenameInhibit = -1;
static int hf_afp_dir_attribute_DeleteInhibit = -1;
static int hf_afp_dir_attribute_SetClear      = -1;

static int hf_afp_file_bitmap_Attributes     = -1;
static int hf_afp_file_bitmap_ParentDirID    = -1;
static int hf_afp_file_bitmap_CreateDate     = -1;
static int hf_afp_file_bitmap_ModDate        = -1;
static int hf_afp_file_bitmap_BackupDate     = -1;
static int hf_afp_file_bitmap_FinderInfo     = -1;
static int hf_afp_file_bitmap_LongName       = -1;
static int hf_afp_file_bitmap_ShortName      = -1;
static int hf_afp_file_bitmap_NodeID         = -1;
static int hf_afp_file_bitmap_DataForkLen    = -1;
static int hf_afp_file_bitmap_RsrcForkLen    = -1;
static int hf_afp_file_bitmap_ExtDataForkLen = -1;
static int hf_afp_file_bitmap_LaunchLimit    = -1;

static int hf_afp_file_bitmap_UTF8Name       = -1;
static int hf_afp_file_bitmap_ExtRsrcForkLen = -1;
static int hf_afp_file_bitmap_UnixPrivs      = -1;

static int hf_afp_file_attribute_Invisible     = -1;
static int hf_afp_file_attribute_MultiUser     = -1;
static int hf_afp_file_attribute_System        = -1;
static int hf_afp_file_attribute_DAlreadyOpen  = -1;
static int hf_afp_file_attribute_RAlreadyOpen  = -1;
static int hf_afp_file_attribute_WriteInhibit  = -1;
static int hf_afp_file_attribute_BackUpNeeded  = -1;
static int hf_afp_file_attribute_RenameInhibit = -1;
static int hf_afp_file_attribute_DeleteInhibit = -1;
static int hf_afp_file_attribute_CopyProtect   = -1;
static int hf_afp_file_attribute_SetClear      = -1;

static const value_string vol_bitmap_vals[] = {
  {kFPVolAttributeBit,          "VolAttribute"},
  {kFPVolSignatureBit,		"VolSignature"},
  {kFPVolCreateDateBit,		"VolCreateDate"},
  {kFPVolModDateBit,		"VolModDate"},
  {kFPVolBackupDateBit,		"VolBackupDate"},
  {kFPVolIDBit,			"VolID"},
  {kFPVolBytesFreeBit,		"VolBytesFree"},
  {kFPVolBytesTotalBit,		"VolBytesTotal"},
  {kFPVolNameBit,		"VolNameBit"},
  {kFPVolExtBytesFreeBit,	"VolExtBytesFree"},
  {kFPVolExtBytesTotalBit,	"VolExtBytesTotal"},
  {kFPVolBlockSizeBit,	  	"VolBlockSize"},
  {0,				 NULL } };

static const value_string flag_vals[] = {
  {0,	"Start" },
  {1,	"End" },
  {0,	NULL } };

static const value_string path_type_vals[] = {
  {1,	"Short names" },
  {2,	"Long names" },
  {3,	"Unicode names" },
  {0,	NULL } };

/*
  volume attribute from Apple AFP3.0.pdf 
  Table 1-3 p. 22
*/
#define kReadOnly 				(1 << 0)
#define kHasVolumePassword 			(1 << 1)
#define kSupportsFileIDs 			(1 << 2)
#define kSupportsCatSearch 			(1 << 3)
#define kSupportsBlankAccessPrivs 		(1 << 4)
#define kSupportsUnixPrivs 			(1 << 5)
#define kSupportsUTF8Names 			(1 << 6)

/*
  directory bitmap from Apple AFP3.0.pdf 
  Table 1-4 p. 31
*/
#define kFPAttributeBit 		(1 << 0)
#define kFPParentDirIDBit 		(1 << 1)
#define kFPCreateDateBit 		(1 << 2)
#define kFPModDateBit 			(1 << 3)
#define kFPBackupDateBit 		(1 << 4)
#define kFPFinderInfoBit 		(1 << 5)
#define kFPLongNameBit			(1 << 6)
#define kFPShortNameBit 		(1 << 7)
#define kFPNodeIDBit 			(1 << 8)
#define kFPOffspringCountBit	 	(1 << 9)
#define kFPOwnerIDBit 			(1 << 10)
#define kFPGroupIDBit 			(1 << 11)
#define kFPAccessRightsBit 		(1 << 12)
#define kFPUTF8NameBit 			(1 << 13)
#define kFPUnixPrivsBit 		(1 << 14)

/*
	directory Access Rights parameter AFP3.0.pdf
	table 1-6 p. 34
*/

#define AR_O_SEARCH	(1 << 0)	/* owner has search access */
#define AR_O_READ	(1 << 1)    /* owner has read access */
#define AR_O_WRITE	(1 << 2)    /* owner has write access */

#define AR_G_SEARCH	(1 << 8)    /* group has search access */
#define AR_G_READ	(1 << 9)    /* group has read access */
#define AR_G_WRITE	(1 << 10)   /* group has write access */

#define AR_E_SEARCH	(1 << 16)	/* everyone has search access */
#define AR_E_READ	(1 << 17)   /* everyone has read access */
#define AR_E_WRITE	(1 << 18)   /* everyone has write access */

#define AR_U_SEARCH	(1 << 24)   /* user has search access */
#define AR_U_READ  	(1 << 25)   /* user has read access */
#define AR_U_WRITE 	(1 << 26)	/* user has write access */ 

#define AR_BLANK	(1 << 28)	/* Blank Access Privileges (use parent dir privileges) */
#define AR_U_OWN 	(1 << 31)	/* user is the owner */

static int hf_afp_dir_ar          = -1;
static int hf_afp_dir_ar_o_search = -1;
static int hf_afp_dir_ar_o_read   = -1;
static int hf_afp_dir_ar_o_write  = -1;
static int hf_afp_dir_ar_g_search = -1;
static int hf_afp_dir_ar_g_read   = -1;
static int hf_afp_dir_ar_g_write  = -1;
static int hf_afp_dir_ar_e_search = -1;
static int hf_afp_dir_ar_e_read   = -1;
static int hf_afp_dir_ar_e_write  = -1;
static int hf_afp_dir_ar_u_search = -1;
static int hf_afp_dir_ar_u_read   = -1;
static int hf_afp_dir_ar_u_write  = -1;
static int hf_afp_dir_ar_blank    = -1;
static int hf_afp_dir_ar_u_own    = -1;

/*
  file bitmap AFP3.0.pdf 
  Table 1-7 p. 36
same as dir
kFPAttributeBit 		(bit 0)
kFPParentDirIDBit 		(bit 1)
kFPCreateDateBit 		(bit 2)
kFPModDateBit 			(bit 3)
kFPBackupDateBit 		(bit 4)
kFPFinderInfoBit 		(bit 5)
kFPLongNameBit 			(bit 6)
kFPShortNameBit 		(bit 7)
kFPNodeIDBit 			(bit 8)

kFPUTF8NameBit 			(bit 13)
*/

#define kFPDataForkLenBit 		(1 << 9)
#define kFPRsrcForkLenBit 		(1 << 10)
#define kFPExtDataForkLenBit 		(1 << 11)
#define kFPLaunchLimitBit 		(1 << 12)

#define kFPExtRsrcForkLenBit 		(1 << 14)
#define kFPUnixPrivsBit_file 		(1 << 15)	/* :( */

/*
  file attribute AFP3.0.pdf 
  Table 1-8 p. 37
*/
#define kFPInvisibleBit 			(1 << 0)
#define kFPMultiUserBit 			(1 << 1)
#define kFPSystemBit 				(1 << 2)
#define kFPDAlreadyOpenBit 			(1 << 3)
#define kFPRAlreadyOpenBit 			(1 << 4)
#define kFPWriteInhibitBit 			(1 << 5)
#define kFPBackUpNeededBit 			(1 << 6)
#define kFPRenameInhibitBit 			(1 << 7)
#define kFPDeleteInhibitBit 			(1 << 8)
#define kFPCopyProtectBit 			(1 << 10)
#define kFPSetClearBit 				(1 << 15)

/* dir attribute */
#define kIsExpFolder 	(1 << 1)
#define kMounted 	(1 << 3)
#define kInExpFolder 	(1 << 4)

#define hash_init_count 20

/* Hash functions */
static gint  afp_equal (gconstpointer v, gconstpointer v2);
static guint afp_hash  (gconstpointer v);
 
static guint afp_packet_init_count = 200;

typedef struct {
	guint32 conversation;
	guint16	seq;
} afp_request_key;
 
typedef struct {
	guint8	command;
} afp_request_val;
 
static GHashTable *afp_request_hash = NULL;
static GMemChunk *afp_request_keys = NULL;
static GMemChunk *afp_request_vals = NULL;

/* Hash Functions */
static gint  afp_equal (gconstpointer v, gconstpointer v2)
{
	afp_request_key *val1 = (afp_request_key*)v;
	afp_request_key *val2 = (afp_request_key*)v2;

	if (val1->conversation == val2->conversation &&
			val1->seq == val2->seq) {
		return 1;
	}
	return 0;
}

static guint afp_hash  (gconstpointer v)
{
        afp_request_key *afp_key = (afp_request_key*)v;
        return afp_key->seq;
}

/* -------------------------- 
*/
#define PAD(x)      { proto_tree_add_item(tree, hf_afp_pad, tvb, offset,  x, FALSE); offset += x; }

static guint16
decode_vol_bitmap (proto_tree *tree, tvbuff_t *tvb, gint offset)
{
  	proto_tree *sub_tree = NULL;
  	proto_item *item;
	guint16  bitmap;

	bitmap = tvb_get_ntohs(tvb, offset);
	if (tree) {
		item = proto_tree_add_item(tree, hf_afp_vol_bitmap, tvb, offset, 2,FALSE);
		sub_tree = proto_item_add_subtree(item, ett_afp_vol_bitmap);
	}
	
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_Attributes,	tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_Signature, 	tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_CreateDate, 	tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_ModDate, 	tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_BackupDate, 	tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_ID, 		tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_BytesFree, 	tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_BytesTotal, 	tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_Name, 		tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_ExtBytesFree, 	tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_ExtBytesTotal, 	tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_bitmap_BlockSize , 	tvb, offset, 2,FALSE);

	return bitmap;
}

/* -------------------------- */
static guint16
decode_vol_attribute (proto_tree *tree, tvbuff_t *tvb, gint offset)
{
  	proto_tree *sub_tree = NULL;
  	proto_item *item;
	guint16  bitmap;

	bitmap = tvb_get_ntohs(tvb, offset);
	if (tree) {
		item = proto_tree_add_item(tree, hf_afp_vol_attribute, tvb, offset, 2,FALSE);
		sub_tree = proto_item_add_subtree(item, ett_afp_vol_attribute);
	}
	proto_tree_add_item(sub_tree, hf_afp_vol_attribute_ReadOnly                ,tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_attribute_HasVolumePassword       ,tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_attribute_SupportsFileIDs         ,tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_attribute_SupportsCatSearch       ,tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_attribute_SupportsBlankAccessPrivs,tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_attribute_SupportsUnixPrivs       ,tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_vol_attribute_SupportsUTF8Names       ,tvb, offset, 2,FALSE);
                                                                               
	return bitmap;                                                             
}                                                                              

/* -------------------------- 
	cf AFP3.0.pdf page 38
	date  are number of seconds from 12:00am on 01.01.2000 GMT
	backup : 0x8000000 not set
	from netatalk adouble.h	
*/
#define DATE_NOT_SET         0x80000000 
#define AD_DATE_DELTA         946684800  
#define AD_DATE_TO_UNIX(x)    (x + AD_DATE_DELTA)  
static guint32
print_date(proto_tree *tree,int id, tvbuff_t *tvb, gint offset)
{
	time_t date = tvb_get_ntohl(tvb, offset);
	nstime_t tv;

	tv.secs = AD_DATE_TO_UNIX(date);
	tv.nsecs = 0;
	proto_tree_add_time(tree, id, tvb, offset, 4, &tv);

	return date;
}

/* -------------------------- */
static gint
parse_vol_bitmap (proto_tree *tree, tvbuff_t *tvb, gint offset, guint16 bitmap)
{
	guint16 nameoff = 0;

	if ((bitmap & kFPVolAttributeBit)) {
		decode_vol_attribute(tree,tvb,offset);
		offset += 2;
	}
	if ((bitmap & kFPVolSignatureBit)) {
		proto_tree_add_item(tree, hf_afp_vol_signature,tvb, offset, 2, FALSE);
		offset += 2;
	}
	if ((bitmap & kFPVolCreateDateBit)) {
		print_date(tree, hf_afp_vol_creation_date,tvb, offset);
		offset += 4;
	}
	if ((bitmap & kFPVolModDateBit)) {
		print_date(tree, hf_afp_vol_modification_date,tvb, offset);
		offset += 4;
	}
	if ((bitmap & kFPVolBackupDateBit)) {
		print_date(tree, hf_afp_vol_backup_date,tvb, offset);
		offset += 4;
	}
	if ((bitmap & kFPVolIDBit)) {
		proto_tree_add_item(tree, hf_afp_vol_id, tvb, offset, 2,FALSE);
		offset += 2;
	}
	if ((bitmap & kFPVolBytesFreeBit)) {
		proto_tree_add_item(tree, hf_afp_vol_bytes_free,tvb, offset, 4, FALSE);
		offset += 4;
	}
	if ((bitmap & kFPVolBytesTotalBit)) {
		proto_tree_add_item(tree, hf_afp_vol_bytes_total,tvb, offset, 4, FALSE);
		offset += 4;
	}
	if ((bitmap & kFPVolNameBit)) {
		nameoff = tvb_get_ntohs(tvb, offset);
		proto_tree_add_item(tree, hf_afp_bitmap_offset,tvb, offset, 2, FALSE);
		offset += 2;

	}
	if ((bitmap & kFPVolExtBytesFreeBit)) {
		proto_tree_add_item(tree, hf_afp_vol_ex_bytes_free,tvb, offset, 8, FALSE);
		offset += 8;
	}
	if ((bitmap & kFPVolExtBytesTotalBit)) {
		proto_tree_add_item(tree, hf_afp_vol_ex_bytes_total,tvb, offset, 8, FALSE);
		offset += 8;
	}
	if ((bitmap & kFPVolBlockSizeBit)) {
		proto_tree_add_item(tree, hf_afp_vol_block_size,tvb, offset, 4, FALSE);
		offset += 4;
	}
	if (nameoff) {
	guint8 len;

		len = tvb_get_guint8(tvb, offset);
		proto_tree_add_item(tree, hf_afp_vol_name, tvb, offset, 1,FALSE);
		offset += len +1;

	}
	return offset;
}

/* -------------------------- */
static guint16
decode_file_bitmap (proto_tree *tree, tvbuff_t *tvb, gint offset)
{
  	proto_tree *sub_tree = NULL;
  	proto_item *item;
	guint16		bitmap;

	bitmap = tvb_get_ntohs(tvb, offset);
	if (tree) {
		item = proto_tree_add_item(tree, hf_afp_file_bitmap, tvb, offset, 2,FALSE);
		sub_tree = proto_item_add_subtree(item, ett_afp_file_bitmap);
	}
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_Attributes      , tvb, offset, 2,FALSE);  
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_ParentDirID    , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_CreateDate     , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_ModDate        , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_BackupDate     , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_FinderInfo     , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_LongName       , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_ShortName      , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_NodeID         , tvb, offset, 2,FALSE);

	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_DataForkLen   	, tvb, offset, 2,FALSE);   
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_RsrcForkLen   	, tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_ExtDataForkLen	, tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_LaunchLimit   	, tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_UTF8Name	    , tvb, offset, 2,FALSE);

	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_ExtRsrcForkLen	, tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_bitmap_UnixPrivs      , tvb, offset, 2,FALSE);

	return bitmap;
}

/* -------------------------- */
static guint16 
decode_file_attribute(proto_tree *tree, tvbuff_t *tvb, gint offset, int shared)
{
  	proto_tree *sub_tree = NULL;
  	proto_item *item;
	guint16		attribute;
	
	attribute = tvb_get_ntohs(tvb, offset);
	if (tree) {
		item = proto_tree_add_text(tree, tvb, offset, 2,
					"File Attributes: 0x%04x", attribute);
		sub_tree = proto_item_add_subtree(item, ett_afp_file_attribute);
	}
	proto_tree_add_item(sub_tree, hf_afp_file_attribute_Invisible    , tvb, offset, 2,FALSE);  
	if (!shared) 
		proto_tree_add_item(sub_tree, hf_afp_file_attribute_MultiUser    , tvb, offset, 2,FALSE);

	proto_tree_add_item(sub_tree, hf_afp_file_attribute_System       , tvb, offset, 2,FALSE);

	if (!shared) {
		proto_tree_add_item(sub_tree, hf_afp_file_attribute_DAlreadyOpen , tvb, offset, 2,FALSE);
		proto_tree_add_item(sub_tree, hf_afp_file_attribute_RAlreadyOpen , tvb, offset, 2,FALSE);
		proto_tree_add_item(sub_tree, hf_afp_file_attribute_WriteInhibit , tvb, offset, 2,FALSE);
	}
	proto_tree_add_item(sub_tree, hf_afp_file_attribute_BackUpNeeded , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_attribute_RenameInhibit, tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_file_attribute_DeleteInhibit, tvb, offset, 2,FALSE);

	if (!shared) 
		proto_tree_add_item(sub_tree, hf_afp_file_attribute_CopyProtect  , tvb, offset, 2,FALSE);

	proto_tree_add_item(sub_tree, hf_afp_file_attribute_SetClear     , tvb, offset, 2,FALSE);

	return(attribute);
}

/* -------------------------- */
static gint
parse_file_bitmap (proto_tree *tree, tvbuff_t *tvb, gint offset, guint16 bitmap, int shared)
{
	guint16 lnameoff = 0;
	guint16 snameoff = 0;
	guint16 unameoff = 0;
	gint 	max_offset = 0;

	gint 	org_offset = offset;

	if ((bitmap & kFPAttributeBit)) {
		decode_file_attribute(tree, tvb, offset, shared);
		offset += 2;
	}
	if ((bitmap & kFPParentDirIDBit)) {
		proto_tree_add_item(tree, hf_afp_did, tvb, offset, 4,FALSE);
		offset += 4;
	}
	if ((bitmap & kFPCreateDateBit)) {
		print_date(tree, hf_afp_creation_date,tvb, offset);
		offset += 4;
	}
	if ((bitmap & kFPModDateBit)) {
		print_date(tree, hf_afp_modification_date,tvb, offset);
		offset += 4;
	}
	if ((bitmap & kFPBackupDateBit)) {
		print_date(tree, hf_afp_backup_date,tvb, offset);
		offset += 4;
	}
	if ((bitmap & kFPFinderInfoBit)) {
		proto_tree_add_item(tree, hf_afp_finder_info,tvb, offset, 32, FALSE);
		offset += 32;
	}
	if ((bitmap & kFPLongNameBit)) {
		gint tp_ofs;
		guint8 len;

		lnameoff = tvb_get_ntohs(tvb, offset);
		if (lnameoff) {
			tp_ofs = lnameoff +org_offset;
			proto_tree_add_item(tree, hf_afp_bitmap_offset,tvb, offset, 2, FALSE);
			len = tvb_get_guint8(tvb, tp_ofs);
			proto_tree_add_item(tree, hf_afp_path_len, tvb, tp_ofs,  1,FALSE);
			tp_ofs++;
			proto_tree_add_item(tree, hf_afp_path_name, tvb, tp_ofs, len,FALSE);
			tp_ofs += len;
			max_offset = (tp_ofs >max_offset)?tp_ofs:max_offset;
		}
		offset += 2;
	}
	if ((bitmap & kFPShortNameBit)) {
		snameoff = tvb_get_ntohs(tvb, offset);
		proto_tree_add_item(tree, hf_afp_bitmap_offset,tvb, offset, 2, FALSE);
		offset += 2;
	}
	if ((bitmap & kFPNodeIDBit)) {
		proto_tree_add_item(tree, hf_afp_file_id, tvb, offset, 4,FALSE);
		offset += 4;
	}

	if ((bitmap & kFPDataForkLenBit)) {
		proto_tree_add_item(tree, hf_afp_file_DataForkLen, tvb, offset, 4,FALSE);
		offset += 4;
	}
 	
	if ((bitmap & kFPRsrcForkLenBit)) {
		proto_tree_add_item(tree, hf_afp_file_RsrcForkLen, tvb, offset, 4,FALSE);
		offset += 4;
	}

	if ((bitmap & kFPExtDataForkLenBit)) {
		proto_tree_add_item(tree, hf_afp_file_ExtDataForkLen, tvb, offset, 8,FALSE);
		offset += 8;
	}

	if ((bitmap & kFPLaunchLimitBit)) {
		offset += 2;	/* ? */
	}

	if ((bitmap & kFPUTF8NameBit)) {
		offset += 2;
	}

	if ((bitmap & kFPExtRsrcForkLenBit)) {
		proto_tree_add_item(tree, hf_afp_file_ExtRsrcForkLen, tvb, offset, 8,FALSE);
		offset += 8;
	}

	if ((bitmap & kFPUnixPrivsBit_file)) {
		proto_tree_add_item(tree, hf_afp_file_UnixPrivs, tvb, offset, 4,FALSE);
		offset += 4;
	}

	return (max_offset)?max_offset:offset;
}

/* -------------------------- */
static guint16 
decode_dir_bitmap (proto_tree *tree, tvbuff_t *tvb, gint offset)
{
  	proto_tree *sub_tree = NULL;
  	proto_item *item;
	guint16		bitmap;
	
	bitmap = tvb_get_ntohs(tvb, offset);
	if (tree) {
		item = proto_tree_add_item(tree, hf_afp_dir_bitmap, tvb, offset, 2,FALSE);
		sub_tree = proto_item_add_subtree(item, ett_afp_dir_bitmap);
	}
	
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_Attributes      , tvb, offset, 2,FALSE);  
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_ParentDirID    , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_CreateDate     , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_ModDate        , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_BackupDate     , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_FinderInfo     , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_LongName       , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_ShortName      , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_NodeID         , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_OffspringCount , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_OwnerID        , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_GroupID        , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_AccessRights   , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_UTF8Name	   , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_bitmap_UnixPrivs      , tvb, offset, 2,FALSE);

	return bitmap;
}

/* -------------------------- */
static guint16 
decode_dir_attribute(proto_tree *tree, tvbuff_t *tvb, gint offset)
{
  	proto_tree *sub_tree = NULL;
  	proto_item *item;
	guint16		attribute;
	
	attribute = tvb_get_ntohs(tvb, offset);
	if (tree) {
		item = proto_tree_add_text(tree, tvb, offset, 2,
					"Directory Attributes: 0x%04x", attribute);
		sub_tree = proto_item_add_subtree(item, ett_afp_dir_attribute);
	}
	proto_tree_add_item(sub_tree, hf_afp_dir_attribute_Invisible    , tvb, offset, 2,FALSE);  
	proto_tree_add_item(sub_tree, hf_afp_dir_attribute_IsExpFolder  , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_attribute_System       , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_attribute_Mounted      , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_attribute_InExpFolder  , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_attribute_BackUpNeeded , tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_attribute_RenameInhibit, tvb, offset, 2,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_dir_attribute_DeleteInhibit, tvb, offset, 2,FALSE);

	return(attribute);
}

/* -------------------------- */
static gint
parse_dir_bitmap (proto_tree *tree, tvbuff_t *tvb, gint offset, guint16 bitmap)
{
	guint16 lnameoff = 0;
	guint16 snameoff = 0;
	guint16 unameoff = 0;
	gint 	max_offset = 0;

	gint 	org_offset = offset;

	if ((bitmap & kFPAttributeBit)) {
		decode_dir_attribute(tree, tvb, offset);
		offset += 2;
	}
	if ((bitmap & kFPParentDirIDBit)) {
		proto_tree_add_item(tree, hf_afp_did, tvb, offset, 4,FALSE);
		offset += 4;
	}
	if ((bitmap & kFPCreateDateBit)) {
		print_date(tree, hf_afp_creation_date,tvb, offset);
		offset += 4;
	}
	if ((bitmap & kFPModDateBit)) {
		print_date(tree, hf_afp_modification_date,tvb, offset);
		offset += 4;
	}
	if ((bitmap & kFPBackupDateBit)) {
		print_date(tree, hf_afp_backup_date,tvb, offset);
		offset += 4;
	}
	if ((bitmap & kFPFinderInfoBit)) {
		proto_tree_add_item(tree, hf_afp_finder_info,tvb, offset, 32, FALSE);
		offset += 32;
	}
	if ((bitmap & kFPLongNameBit)) {
		gint tp_ofs;
		guint8 len;
		lnameoff = tvb_get_ntohs(tvb, offset);
		if (lnameoff) {
			tp_ofs = lnameoff +org_offset;
			proto_tree_add_item(tree, hf_afp_bitmap_offset,tvb, offset, 2, FALSE);
			len = tvb_get_guint8(tvb, tp_ofs);
			proto_tree_add_item(tree, hf_afp_path_len, tvb, tp_ofs,  1,FALSE);
			tp_ofs++;
			proto_tree_add_item(tree, hf_afp_path_name, tvb, tp_ofs, len,FALSE);
			tp_ofs += len;
			max_offset = (tp_ofs >max_offset)?tp_ofs:max_offset;
		}
		offset += 2;
	}
	if ((bitmap & kFPShortNameBit)) {
		snameoff = tvb_get_ntohs(tvb, offset);
		proto_tree_add_item(tree, hf_afp_bitmap_offset,tvb, offset, 2, FALSE);
		offset += 2;
	}
	if ((bitmap & kFPNodeIDBit)) {
		proto_tree_add_item(tree, hf_afp_file_id, tvb, offset, 4,FALSE);
		offset += 4;
	}
	if ((bitmap & kFPOffspringCountBit)) {
		proto_tree_add_item(tree, hf_afp_dir_offspring, tvb, offset, 2,FALSE);
		offset += 2;		/* error in AFP3.0.pdf */
	}
	if ((bitmap & kFPOwnerIDBit)) {
		proto_tree_add_item(tree, hf_afp_dir_OwnerID, tvb, offset, 4,	FALSE);  
		offset += 4;
	}
	if ((bitmap & kFPGroupIDBit)) {
		proto_tree_add_item(tree, hf_afp_dir_GroupID, tvb, offset, 4,	FALSE);  
		offset += 4;
	}
	if ((bitmap & kFPAccessRightsBit)) {
  		proto_tree *sub_tree = NULL;
  		proto_item *item;
  			
		if (tree) {
			item = proto_tree_add_item(tree, hf_afp_dir_ar, tvb, offset, 4, FALSE);
			sub_tree = proto_item_add_subtree(item, ett_afp_dir_ar);
		}
		proto_tree_add_item(sub_tree, hf_afp_dir_ar_o_search, tvb, offset, 4,	FALSE);  
		proto_tree_add_item(sub_tree, hf_afp_dir_ar_o_read  , tvb, offset, 4,	FALSE);  
		proto_tree_add_item(sub_tree, hf_afp_dir_ar_o_write , tvb, offset, 4,	FALSE);  

		proto_tree_add_item(sub_tree, hf_afp_dir_ar_g_search, tvb, offset, 4,	FALSE);  
		proto_tree_add_item(sub_tree, hf_afp_dir_ar_g_read  , tvb, offset, 4,	FALSE);  
		proto_tree_add_item(sub_tree, hf_afp_dir_ar_g_write , tvb, offset, 4,	FALSE);  

		proto_tree_add_item(sub_tree, hf_afp_dir_ar_e_search, tvb, offset, 4,	FALSE);  
		proto_tree_add_item(sub_tree, hf_afp_dir_ar_e_read  , tvb, offset, 4,	FALSE);  
		proto_tree_add_item(sub_tree, hf_afp_dir_ar_e_write , tvb, offset, 4,	FALSE);  

		proto_tree_add_item(sub_tree, hf_afp_dir_ar_u_search, tvb, offset, 4,	FALSE);  
		proto_tree_add_item(sub_tree, hf_afp_dir_ar_u_read  , tvb, offset, 4,	FALSE);  
		proto_tree_add_item(sub_tree, hf_afp_dir_ar_u_write , tvb, offset, 4,	FALSE);  

		proto_tree_add_item(sub_tree, hf_afp_dir_ar_blank   , tvb, offset, 4,	FALSE);  
		proto_tree_add_item(sub_tree, hf_afp_dir_ar_u_own   , tvb, offset, 4,	FALSE);  
		
		offset += 4;
	}
	if ((bitmap & kFPUTF8NameBit)) {
		offset += 2;
	}
	if ((bitmap & kFPUnixPrivsBit)) {
		offset += 4;
	}
	return (max_offset)?max_offset:offset;
}

/* -------------------------- */
static gchar *
name_in_bitmap(tvbuff_t *tvb, gint *offset, guint16 bitmap)
{
	gchar *name;
	gint 	org_offset = *offset;
	guint16 nameoff;
	guint8  len;
	gint	tp_ofs;
	
	name = NULL;
	if ((bitmap & kFPAttributeBit)) 
		*offset += 2;
	if ((bitmap & kFPParentDirIDBit))
		*offset += 4;
	if ((bitmap & kFPCreateDateBit)) 
		*offset += 4;
	if ((bitmap & kFPModDateBit))
		*offset += 4;
	if ((bitmap & kFPBackupDateBit)) 
		*offset += 4;
	if ((bitmap & kFPFinderInfoBit)) 
		*offset += 32;
	
	if ((bitmap & kFPLongNameBit)) {
		nameoff = tvb_get_ntohs(tvb, *offset);
		if (nameoff) {
			tp_ofs = nameoff +org_offset;
			len = tvb_get_guint8(tvb, tp_ofs);
			tp_ofs++;
			if (!(name = g_malloc(len +1)))
				return name;
			tvb_memcpy(tvb, name, tp_ofs, len);
			*(name +len) = 0;
			return name;
		}
	}
	/* short name ? */
	return name;
}

/* -------------------------- */
static gchar *
name_in_dbitmap(tvbuff_t *tvb, gint offset, guint16 bitmap)
{
	gchar *name;
	
	name = name_in_bitmap(tvb, &offset, bitmap);
	if (name != NULL)
		return name;
	/*
		check UTF8 name 
	*/
	
	return name;
}

/* -------------------------- */
static gchar *
name_in_fbitmap(tvbuff_t *tvb, gint offset, guint16 bitmap)
{
	gchar *name;
	
	name = name_in_bitmap(tvb, &offset, bitmap);
	if (name != NULL)
		return name;
	/*
		check UTF8 name 
	*/
	
	return name;
}

/* -------------------------- */
static gint
decode_vol_did(proto_tree *tree, tvbuff_t *tvb, gint offset)
{
	proto_tree_add_item(tree, hf_afp_vol_id, tvb, offset, 2,FALSE);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_did, tvb, offset, 4,FALSE);
	offset += 4;
	return offset;
}

/* -------------------------- */
static gint
decode_vol_did_file_dir_bitmap (proto_tree *tree, tvbuff_t *tvb, gint offset)
{
	offset = decode_vol_did(tree, tvb, offset);

	decode_file_bitmap(tree, tvb, offset);
	offset += 2;
	
	decode_dir_bitmap(tree, tvb, offset);
	offset += 2;
	
	return offset;
}

/* ------------------------
 * idea from packet-smb
 *
 */
static gchar *
get_name(tvbuff_t *tvb, int offset, int type)
{
  	static gchar  str[3][256];
  	static int    cur;
  	gchar *string;
  	guint8 len;
	int i;
	len = tvb_get_guint8(tvb, offset);
	offset++;
	string = str[cur];
	switch (type) {
	case 1:
	case 2:
    	tvb_memcpy(tvb, (guint8 *)string, offset, len);
    	string[len] = 0;
		/* FIXME should use something else as separator ?
		*/
    	for (i = 0; i < len; i++) if (!string[i])
    		string[i] = ':';	
    	break;
    case 3:
    	strcpy(string, "error Unicode...,next time ");
    	break;
    }

  	cur = ++cur % 3;
  	return string;
}

/* -------------------------- */
static gint
decode_name_label (proto_tree *tree, packet_info *pinfo, tvbuff_t *tvb, gint offset, const gchar *label)
{
	int len;
	gchar *name;
	guint8 type;
  	proto_tree *sub_tree = NULL;
  	proto_item *item;
		
	type = tvb_get_guint8(tvb, offset);
	len = tvb_get_guint8(tvb, offset +1);
	name = get_name(tvb, offset +1, type);

	if (pinfo && check_col(pinfo->cinfo, COL_INFO)) {
		col_append_fstr(pinfo->cinfo, COL_INFO, ": %s", name);
	}

	if (tree) {
		item = proto_tree_add_text(tree, tvb, offset, len +2, label, name);
		sub_tree = proto_item_add_subtree(item, ett_afp_path_name);
		proto_tree_add_item(  sub_tree, hf_afp_path_type, tvb, offset,   1,FALSE);
		offset++;
		proto_tree_add_item(  sub_tree, hf_afp_path_len,  tvb, offset,   1,FALSE);
		offset++;
		proto_tree_add_string(sub_tree, hf_afp_path_name, tvb, offset, len,name);
	}
	else 
		offset += 2;

	return offset +len;
}

/* -------------------------- */
static gint
decode_name (proto_tree *tree, packet_info *pinfo, tvbuff_t *tvb, gint offset)
{
	return decode_name_label(tree, pinfo, tvb, offset, "Path: %s");
}

/* ************************** */
static gint
dissect_query_afp_open_vol(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	int len;
	
	PAD(1);

	decode_vol_bitmap(tree, tvb, offset);
	offset += 2;
	
	len = tvb_get_guint8(tvb, offset);

	if (check_col(pinfo->cinfo, COL_INFO)) {
	const gchar *rep;
		rep = get_name(tvb, offset, 2);
		col_append_fstr(pinfo->cinfo, COL_INFO, ": %s", rep);
	}

	if (!tree)
		return offset;
		
	proto_tree_add_item(tree, hf_afp_vol_name, tvb, offset, 1,FALSE);
	offset += len +1;
		
  	len = tvb_reported_length_remaining(tvb,offset);
  	if (len >= 8) {
		/* optionnal password */
		proto_tree_add_item(tree, hf_afp_passwd, tvb, offset, 8,FALSE);
		offset += 8;
	}
	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_open_vol(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	guint16 bitmap;
	
	if (!tree)
		return offset;
	bitmap = decode_vol_bitmap(tree, tvb, offset);
	offset += 2;
	offset = parse_vol_bitmap(tree, tvb, offset, bitmap);

	return offset;
}

/* ************************** 
	next calls use the same format :
		1 pad byte 
		volume id
	AFP_FLUSH
	AFP_CLOSEVOL
	AFP_OPENDT
*/
static gint
dissect_query_afp_with_vol_id(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	if (!tree)
		return offset;
	PAD(1);
	
	proto_tree_add_item(tree, hf_afp_vol_id, tvb, offset, 2,FALSE);
	offset += 2;
	return offset;
}

/* ************************** */
static gint
dissect_query_afp_open_fork(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
  	proto_tree *sub_tree = NULL;
  	proto_item *item;

	proto_tree_add_item(tree, hf_afp_fork_type, tvb, offset, 1,FALSE);
	offset++;

	offset = decode_vol_did(tree, tvb, offset);
	
	decode_file_bitmap(tree, tvb, offset);
	offset += 2;
	if (tree) {
		item = proto_tree_add_item(tree, hf_afp_access_mode, tvb, offset, 2,FALSE);
		sub_tree = proto_item_add_subtree(item, ett_afp_access_mode);

		proto_tree_add_item(sub_tree, hf_afp_access_read      , tvb, offset, 2,FALSE);
		proto_tree_add_item(sub_tree, hf_afp_access_write     , tvb, offset, 2,FALSE);
		proto_tree_add_item(sub_tree, hf_afp_access_deny_read , tvb, offset, 2,FALSE);
		proto_tree_add_item(sub_tree, hf_afp_access_deny_write, tvb, offset, 2,FALSE);
	}
	offset += 2;

	offset = decode_name(tree, pinfo, tvb, offset);

	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_open_fork(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	int f_bitmap;

	f_bitmap = decode_file_bitmap(tree, tvb, offset);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_ofork, tvb, offset, 2,FALSE);
	offset += 2;

	offset = parse_file_bitmap(tree, tvb, offset, f_bitmap,0);

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_enumerate(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	
	PAD(1);
	offset = decode_vol_did_file_dir_bitmap(tree, tvb, offset);

	proto_tree_add_item(tree, hf_afp_req_count, tvb, offset, 2,FALSE);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_start_index, tvb, offset, 2,FALSE);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_max_reply_size, tvb, offset, 2,FALSE);
	offset += 2;

	offset = decode_name(tree, pinfo, tvb, offset);

	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_enumerate(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
  	proto_tree *sub_tree = NULL;
  	proto_item *item;
	int count;
	int f_bitmap;
	int d_bitmap;
	guint8	flags;
	guint8	size;
	gint	org;
	int i;
	gchar *name;
	
	f_bitmap = decode_file_bitmap(tree, tvb, offset);
	offset += 2;
	
	d_bitmap = decode_dir_bitmap(tree, tvb, offset);
	offset += 2;

	count = tvb_get_ntohs(tvb, offset);
	if (tree) {
		item = proto_tree_add_item(tree, hf_afp_req_count, tvb, offset, 2,FALSE);
		sub_tree = proto_item_add_subtree(item, ett_afp_enumerate);
	}
	offset += 2;
	/* loop */
	for (i = 0; i < count; i++) {
		org = offset;
		name = NULL;
		size = tvb_get_guint8(tvb, offset);
		flags = tvb_get_guint8(tvb, offset +1);

		if (sub_tree) {
			if (flags) {
				name = name_in_dbitmap(tvb, offset +2, d_bitmap);
			}	
			else {
				name = name_in_fbitmap(tvb, offset +2, f_bitmap);
			}
			if (!name) {
				if (!(name = g_malloc(50))) { /* no memory ! */
				}
				snprintf(name, 50,"line %d", i +1);
			}
			item = proto_tree_add_text(sub_tree, tvb, offset, size, name);
			tree = proto_item_add_subtree(item, ett_afp_enumerate_line);
		}
		proto_tree_add_item(tree, hf_afp_struct_size, tvb, offset, 1,FALSE);
		offset++;

		proto_tree_add_item(tree, hf_afp_file_flag, tvb, offset, 1,FALSE);
		offset++;
		if (flags) {
			offset = parse_dir_bitmap(tree, tvb, offset, d_bitmap);
		}
		else {
			offset = parse_file_bitmap(tree, tvb, offset, f_bitmap,0);
		}
		if ((offset & 1)) 
			PAD(1);
		offset = org +size;		/* play safe */
		if (sub_tree)
			g_free((gpointer)name);
	}	
	return(offset);

}

/* **************************/
static gint
dissect_query_afp_get_vol_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	PAD(1)
	proto_tree_add_item(tree, hf_afp_vol_id, tvb, offset, 2,FALSE);
	offset += 2;

	decode_vol_bitmap(tree, tvb, offset);
	offset += 2;
	
	return offset;	
}

/* ------------------------ */
static gint
dissect_reply_afp_get_vol_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	guint16 bitmap;

	bitmap = decode_vol_bitmap(tree, tvb, offset);
	offset += 2;

	offset = parse_vol_bitmap(tree, tvb, offset, bitmap);

	return offset;
}

/* **************************/
static gint
dissect_query_afp_set_vol_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	guint16 bitmap;

	PAD(1)
	proto_tree_add_item(tree, hf_afp_vol_id, tvb, offset, 2,FALSE);
	offset += 2;

	bitmap = decode_vol_bitmap(tree, tvb, offset);
	offset += 2;
	
	offset = parse_vol_bitmap(tree, tvb, offset, bitmap);

	return offset;	
}

/* ***************************/
static gint
dissect_query_afp_login(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	int len;
	const char *uam;
    
	len = tvb_get_guint8(tvb, offset);
	proto_tree_add_item(tree, hf_afp_AFPVersion, tvb, offset, 1,FALSE);
	offset += len +1;
	len = tvb_get_guint8(tvb, offset);
	uam = tvb_get_ptr(tvb, offset +1, len);
	proto_tree_add_item(tree, hf_afp_UAM, tvb, offset, 1,FALSE);
	offset += len +1;

	if (!strncmp(uam, "Cleartxt passwrd", len)) {
		/* clear text */
		len = tvb_get_guint8(tvb, offset);
		proto_tree_add_item(tree, hf_afp_user, tvb, offset, 1,FALSE);
		offset += len +1;

		len = tvb_strsize(tvb, offset);
		proto_tree_add_item(tree, hf_afp_passwd, tvb, offset, len,FALSE);
		offset += len;
	}
	else if (!strncmp(uam, "No User Authent", len)) {
	}
	return(offset);
}

/* ************************** */
static gint
dissect_query_afp_write(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	int len;
	
	proto_tree_add_item(tree, hf_afp_flag, tvb, offset, 1,FALSE);
	offset += 1;

	proto_tree_add_item(tree, hf_afp_ofork, tvb, offset, 2,FALSE);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_offset, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_rw_count, tvb, offset, 4,FALSE);
	offset += 4;

	return offset;
}

static gint
dissect_reply_afp_write(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	proto_tree_add_item(tree, hf_afp_last_written, tvb, offset, 4, FALSE);
	offset += 4;
	
	return offset;
}

/* ************************** */
static gint
dissect_query_afp_read(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	int len;
	
	PAD(1);
	
	proto_tree_add_item(tree, hf_afp_ofork, tvb, offset, 2,FALSE);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_offset, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_rw_count, tvb, offset, 4,FALSE);
	offset += 4;

	/* FIXME */
	offset++;
	
	offset++;
	return offset;
}

/* ************************** 
   Open desktop call 
   query is the same than 	AFP_FLUSH, AFP_CLOSEVOL

*/
static gint
dissect_reply_afp_open_dt(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	proto_tree_add_item(tree, hf_afp_dt_ref, tvb, offset, 2,FALSE);
	offset += 2;

	return offset;
}

/* ************************** 
	no reply
*/
static gint
dissect_query_afp_close_dt(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	PAD(1);
	proto_tree_add_item(tree, hf_afp_dt_ref, tvb, offset, 2,FALSE);
	offset += 2;

	return offset;
}

/* ************************** 
	calls using the same format :
		1 pad byte 
		fork number 
	AFP_FLUSHFORK
	AFP_CLOSEFORK
*/
static gint
dissect_query_afp_with_fork(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	PAD(1);
	proto_tree_add_item(tree, hf_afp_ofork, tvb, offset, 2,FALSE);
	offset += 2;

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_get_fldr_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	PAD(1);
	offset = decode_vol_did_file_dir_bitmap(tree, tvb, offset);

	offset = decode_name(tree, pinfo, tvb, offset);

	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_get_fldr_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	guint8	flags;
	guint16 f_bitmap, d_bitmap;

	f_bitmap = decode_file_bitmap(tree, tvb, offset);
	offset += 2;
	
	d_bitmap = decode_dir_bitmap(tree, tvb, offset);
	offset += 2;

	flags = tvb_get_guint8(tvb, offset);
	proto_tree_add_item(tree, hf_afp_file_flag, tvb, offset, 1,FALSE);
	offset++;
	PAD(1);
	if (flags) {
		offset = parse_dir_bitmap(tree, tvb, offset, d_bitmap);
	}
	else {
		offset = parse_file_bitmap(tree, tvb, offset, f_bitmap,0);
	}
	return offset;
}

/* ************************** 
	no reply
*/
static gint
dissect_query_afp_set_fldr_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	guint16	f_bitmap;
	
	PAD(1);
	offset = decode_vol_did(tree, tvb, offset);

	f_bitmap = decode_file_bitmap(tree, tvb, offset);
	offset += 2;
	
	offset = decode_name(tree, pinfo, tvb, offset);

	if ((offset & 1))
		PAD(1);
	/* did:name can be a file or a folder but only the intersection between 
	 * file bitmap and dir bitmap can be set
	 */
	offset = parse_file_bitmap(tree, tvb, offset, f_bitmap, 1);

	return offset;
}

/* ************************** 
	no reply
*/
static gint
dissect_query_afp_set_file_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	guint16	f_bitmap;
	
	PAD(1);
	offset = decode_vol_did(tree, tvb, offset);

	f_bitmap = decode_file_bitmap(tree, tvb, offset);
	offset += 2;
	
	offset = decode_name(tree, pinfo, tvb, offset);

	if ((offset & 1))
		PAD(1);
	offset = parse_file_bitmap(tree, tvb, offset, f_bitmap, 0);

	return offset;
}

/* ************************** 
	no reply
*/
static gint
dissect_query_afp_set_dir_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	guint16	d_bitmap;

	PAD(1);
	offset = decode_vol_did(tree, tvb, offset);

	d_bitmap = decode_dir_bitmap(tree, tvb, offset);
	offset += 2;
	
	offset = decode_name(tree, pinfo, tvb, offset);

	if ((offset & 1))
		PAD(1);
	offset = parse_dir_bitmap(tree, tvb, offset, d_bitmap);

	offset += 4;
	return offset;
}

/* **************************
	AFP_DELETE
	AFP_CREATE_DIR
 */
static gint
dissect_query_afp_create_id(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	PAD(1);
	offset = decode_vol_did(tree, tvb, offset);

	offset = decode_name(tree, pinfo, tvb, offset);
	return offset;
}

/* -------------------------- 
	AFP_MOVE
*/
static gint
dissect_reply_afp_create_id(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	proto_tree_add_item(tree, hf_afp_file_id, tvb, offset, 4,FALSE);
	offset += 4;
	
	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_create_dir(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	proto_tree_add_item(tree, hf_afp_did, tvb, offset, 4,FALSE);
	offset += 4;
	
	return offset;
}

/* ************************** 
	no reply
*/
static gint
dissect_query_afp_delete_id(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	PAD(1);
	proto_tree_add_item(tree, hf_afp_vol_id, tvb, offset, 2,FALSE);
	offset += 2;
	proto_tree_add_item(tree, hf_afp_file_id, tvb, offset, 4,FALSE);
	offset += 4;
	
	return offset;
}

/* ************************** 
	same reply as get_fork_param
*/
static gint
dissect_query_afp_resolve_id(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	PAD(1);
	proto_tree_add_item(tree, hf_afp_vol_id, tvb, offset, 2,FALSE);
	offset += 2;
	proto_tree_add_item(tree, hf_afp_file_id, tvb, offset, 4,FALSE);
	offset += 4;

	decode_file_bitmap(tree, tvb, offset);
	offset += 2;

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_get_fork_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	PAD(1);
	proto_tree_add_item(tree, hf_afp_ofork, tvb, offset, 2,FALSE);
	offset += 2;

	decode_file_bitmap(tree, tvb, offset);
	offset += 2;
	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_get_fork_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	guint16 f_bitmap;

	f_bitmap = decode_file_bitmap(tree, tvb, offset);
	offset += 2;

	offset = parse_file_bitmap(tree, tvb, offset, f_bitmap,0);

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_set_fork_param(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	PAD(1);
	proto_tree_add_item(tree, hf_afp_ofork, tvb, offset, 2,FALSE);
	offset += 2;

	decode_file_bitmap(tree, tvb, offset);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_ofork_len, tvb, offset, 4,FALSE);
	offset += 4;
	return offset;
}

/* ************************** */
static gint
dissect_query_afp_move(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
		
	PAD(1);
	offset = decode_vol_did(tree, tvb, offset);

	proto_tree_add_item(tree, hf_afp_did, tvb, offset, 4,FALSE);
	offset += 4;

	offset = decode_name_label(tree, pinfo, tvb, offset, "Source path: %s");
	offset = decode_name_label(tree, NULL, tvb, offset,  "Dest dir:    %s");
	offset = decode_name_label(tree, NULL, tvb, offset,  "New name:    %s");

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_rename(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
		
	PAD(1);
	offset = decode_vol_did(tree, tvb, offset);

	offset = decode_name_label(tree, pinfo, tvb, offset, "Old name:     %s");
	offset = decode_name_label(tree, NULL, tvb, offset,  "New name:     %s");

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_byte_lock(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
  	proto_tree *sub_tree = NULL;
  	proto_item *item;
	guint8 flag;
	
	flag = tvb_get_guint8(tvb, offset);
	if (tree) {
		item = proto_tree_add_text(tree, tvb, offset, 1, "Flags: 0x%02x", flag);
		sub_tree = proto_item_add_subtree(item, ett_afp_lock_flags);
	}

	proto_tree_add_item(sub_tree, hf_afp_lock_op, tvb, offset, 1,FALSE);
	proto_tree_add_item(sub_tree, hf_afp_lock_from, tvb, offset, 1,FALSE);
	offset += 1;

	proto_tree_add_item(tree, hf_afp_ofork, tvb, offset, 2,FALSE);
	offset += 2;
	
	proto_tree_add_item(tree, hf_afp_lock_offset, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_lock_len, tvb, offset, 4,FALSE);
	offset += 4;
	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_byte_lock(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	proto_tree_add_item(tree, hf_afp_lock_range_start, tvb, offset, 4,FALSE);
	offset += 4;

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_add_cmt(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	guint8 len;

	PAD(1);
	proto_tree_add_item(tree, hf_afp_dt_ref, tvb, offset, 2,FALSE);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_did, tvb, offset, 4,FALSE);
	offset += 4;

	offset = decode_name(tree, pinfo, tvb, offset);

	if ((offset & 1)) 
		PAD(1);

	len = tvb_get_guint8(tvb, offset);
	proto_tree_add_item(tree, hf_afp_comment, tvb, offset, 1,FALSE);
	offset += len +1;

	return offset;
}


/* ************************** */
static gint
dissect_query_afp_get_cmt(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	PAD(1);
	proto_tree_add_item(tree, hf_afp_dt_ref, tvb, offset, 2,FALSE);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_did, tvb, offset, 4,FALSE);
	offset += 4;

	offset = decode_name(tree, pinfo, tvb, offset);
	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_get_cmt(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	guint8 len;

	len = tvb_get_guint8(tvb, offset);
	proto_tree_add_item(tree, hf_afp_comment, tvb, offset, 1,FALSE);
	offset += len +1;

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_get_icon(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	PAD(1);
	proto_tree_add_item(tree, hf_afp_dt_ref, tvb, offset, 2,FALSE);
	offset += 2;
	proto_tree_add_item(tree, hf_afp_file_creator, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_file_type, tvb, offset, 4,FALSE);
	offset += 4;
	
	proto_tree_add_item(tree, hf_afp_icon_type, tvb, offset, 1,FALSE);
	offset += 1;
	PAD(1);

	proto_tree_add_item(tree, hf_afp_icon_length, tvb, offset, 2,FALSE);
	offset += 2;

	return offset;
}

/* -------------------------- 
	fallback to data in afp dissector
*/
static gint
dissect_reply_afp_get_icon(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	return offset;
}

/* ************************** */
static gint
dissect_query_afp_get_icon_info(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	PAD(1);
	proto_tree_add_item(tree, hf_afp_dt_ref, tvb, offset, 2,FALSE);
	offset += 2;
	proto_tree_add_item(tree, hf_afp_file_creator, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_icon_index, tvb, offset, 2,FALSE);
	offset += 2;

	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_get_icon_info(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	proto_tree_add_item(tree, hf_afp_icon_tag, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_file_type, tvb, offset, 4,FALSE);
	offset += 4;
	
	proto_tree_add_item(tree, hf_afp_icon_type, tvb, offset, 1,FALSE);
	offset += 1;

	PAD(1);
	proto_tree_add_item(tree, hf_afp_icon_length, tvb, offset, 2,FALSE);
	offset += 2;

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_add_icon(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	PAD(1);
	proto_tree_add_item(tree, hf_afp_dt_ref, tvb, offset, 2,FALSE);
	offset += 2;
	proto_tree_add_item(tree, hf_afp_file_creator, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_file_type, tvb, offset, 4,FALSE);
	offset += 4;
	
	proto_tree_add_item(tree, hf_afp_icon_type, tvb, offset, 1,FALSE);
	offset += 1;
	
	PAD(1);
	proto_tree_add_item(tree, hf_afp_icon_tag, tvb, offset, 4,FALSE);
	offset += 4;
	
	proto_tree_add_item(tree, hf_afp_icon_length, tvb, offset, 2,FALSE);
	offset += 2;

	return offset;
}

/* ************************** 
	no reply
*/
static gint
dissect_query_afp_add_appl(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	PAD(1);
	proto_tree_add_item(tree, hf_afp_dt_ref, tvb, offset, 2,FALSE);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_did, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_file_creator, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_appl_tag, tvb, offset, 4,FALSE);
	offset += 4;
	
	offset = decode_name(tree, pinfo, tvb, offset);

	return offset;
}

/* ************************** 
	no reply
*/
static gint
dissect_query_afp_rmv_appl(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	PAD(1);
	proto_tree_add_item(tree, hf_afp_dt_ref, tvb, offset, 2,FALSE);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_did, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_file_creator, tvb, offset, 4,FALSE);
	offset += 4;

	offset = decode_name(tree, pinfo, tvb, offset);

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_get_appl(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{

	PAD(1);
	proto_tree_add_item(tree, hf_afp_dt_ref, tvb, offset, 2,FALSE);
	offset += 2;

	proto_tree_add_item(tree, hf_afp_file_creator, tvb, offset, 4,FALSE);
	offset += 4;

	proto_tree_add_item(tree, hf_afp_appl_index, tvb, offset, 2,FALSE);
	offset += 2;

	decode_file_bitmap(tree, tvb, offset);
	offset += 2;

	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_get_appl(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	proto_tree_add_item(tree, hf_afp_appl_tag, tvb, offset, 4,FALSE);
	offset += 4;

	return offset;
}

/* ************************** */
static gint
dissect_query_afp_create_file(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	proto_tree_add_item(tree, hf_afp_create_flag, tvb, offset, 1,FALSE);
	offset++;

	offset = decode_vol_did(tree, tvb, offset);

	offset = decode_name(tree, pinfo, tvb, offset);

	return offset;
}

/* -------------------------- */
static gint
dissect_reply_afp_create_file(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, gint offset)
{
	return offset;
}

/* ************************** */
static void
dissect_afp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	struct aspinfo *aspinfo = pinfo->private_data;
	proto_tree      *afp_tree = NULL;
	proto_item	*ti;
	conversation_t	*conversation;
	gint		offset = 0;
	afp_request_key request_key, *new_request_key;
	afp_request_val *request_val;

	gchar	*func_str;

	guint8	afp_command;
	guint16 afp_requestid;
	gint32 	afp_code;
	guint32 afp_length;
	guint32 afp_reserved;
	int     len =  tvb_reported_length_remaining(tvb,0);
	
	if (check_col(pinfo->cinfo, COL_PROTOCOL))
		col_set_str(pinfo->cinfo, COL_PROTOCOL, "AFP");
	if (check_col(pinfo->cinfo, COL_INFO))
		col_clear(pinfo->cinfo, COL_INFO);

	conversation = find_conversation(&pinfo->src, &pinfo->dst, pinfo->ptype,
		pinfo->srcport, pinfo->destport, 0);

	if (conversation == NULL)
	{
		conversation = conversation_new(&pinfo->src, &pinfo->dst,
			pinfo->ptype, pinfo->srcport, pinfo->destport, 0);
	}

	request_key.conversation = conversation->index;	
	request_key.seq = aspinfo->seq;

	request_val = (afp_request_val *) g_hash_table_lookup(
								afp_request_hash, &request_key);

	if (!request_val && !aspinfo->reply)  {
		afp_command = tvb_get_guint8(tvb, offset);
		new_request_key = g_mem_chunk_alloc(afp_request_keys);
		*new_request_key = request_key;

		request_val = g_mem_chunk_alloc(afp_request_vals);
		request_val->command = tvb_get_guint8(tvb, offset);

		g_hash_table_insert(afp_request_hash, new_request_key,
								request_val);
	}

	if (!request_val) {	/* missing request */
		return;
	}

	afp_command = request_val->command;
	if (check_col(pinfo->cinfo, COL_INFO)) {
		col_add_fstr(pinfo->cinfo, COL_INFO, "%s %s",
			     val_to_str(afp_command, CommandCode_vals,
					"Unknown command (0x%02x)"),
			     aspinfo->reply ? "reply" : "request");
	}

	if (tree)
	{
		ti = proto_tree_add_item(tree, proto_afp, tvb, offset, -1,FALSE);
		afp_tree = proto_item_add_subtree(ti, ett_afp);
	}
	if (!aspinfo->reply)  {
		proto_tree_add_uint(afp_tree, hf_afp_command, tvb,offset, 1, afp_command);
		offset++;
		switch(afp_command) {
		case AFP_BYTELOCK:
			offset = dissect_query_afp_byte_lock(tvb, pinfo, afp_tree, offset);break; 
		case AFP_OPENDT: 	/* same as close vol */
		case AFP_FLUSH:		
		case AFP_CLOSEVOL:
			offset = dissect_query_afp_with_vol_id(tvb, pinfo, afp_tree, offset);break;
		case AFP_CLOSEDIR:
			/* offset = dissect_query_afp_close_dir(tvb, pinfo, afp_tree, offset);break; */
			break;
		case AFP_CLOSEDT:	
			offset = dissect_query_afp_close_dt(tvb, pinfo, afp_tree, offset);break;
		case AFP_FLUSHFORK: /* same packet as closefork */
		case AFP_CLOSEFORK:
			offset = dissect_query_afp_with_fork(tvb, pinfo, afp_tree, offset);break;
		case AFP_COPYFILE:
			/* offset = dissect_query_afp_copy_file(tvb, pinfo, afp_tree, offset);break; */
		case AFP_CREATEFILE:
			offset = dissect_query_afp_create_file(tvb, pinfo, afp_tree, offset);break; 
		case AFP_ENUMERATE:
			offset = dissect_query_afp_enumerate(tvb, pinfo, afp_tree, offset);break;
		case AFP_GETFORKPARAM:
			offset = dissect_query_afp_get_fork_param(tvb, pinfo, afp_tree, offset);break; 
		case AFP_GETSRVINFO:
			/* offset = dissect_query_afp_get_server_info(tvb, pinfo, afp_tree, offset);break; */
		case AFP_GETSRVPARAM:
			break;					/* no parameters */
		case AFP_GETVOLPARAM:
			offset = dissect_query_afp_get_vol_param(tvb, pinfo, afp_tree, offset);break;
		case AFP_LOGIN:
			offset = dissect_query_afp_login(tvb, pinfo, afp_tree, offset);break;
		case AFP_LOGINCONT:
		case AFP_LOGOUT:
		case AFP_MAPID:
		case AFP_MAPNAME:
			break;
		case AFP_MOVE:
			offset = dissect_query_afp_move(tvb, pinfo, afp_tree, offset);break;
		case AFP_OPENVOL:
			offset = dissect_query_afp_open_vol(tvb, pinfo, afp_tree, offset);break;
		case AFP_OPENDIR:
			break;
		case AFP_OPENFORK:
			offset = dissect_query_afp_open_fork(tvb, pinfo, afp_tree, offset);break;
		case AFP_READ:
			offset = dissect_query_afp_read(tvb, pinfo, afp_tree, offset);break;
		case AFP_RENAME:
			offset = dissect_query_afp_rename(tvb, pinfo, afp_tree, offset);break;
		case AFP_SETDIRPARAM:
			offset = dissect_query_afp_set_dir_param(tvb, pinfo, afp_tree, offset);break; 
		case AFP_SETFILEPARAM:
			offset = dissect_query_afp_set_file_param(tvb, pinfo, afp_tree, offset);break; 
		case AFP_SETFORKPARAM:
			offset = dissect_query_afp_set_fork_param(tvb, pinfo, afp_tree, offset);break; 
		case AFP_SETVOLPARAM:
			offset = dissect_query_afp_set_vol_param(tvb, pinfo, afp_tree, offset);break;
		case AFP_WRITE:
			offset = dissect_query_afp_write(tvb, pinfo, afp_tree, offset);break;
		case AFP_GETFLDRPARAM:
			offset = dissect_query_afp_get_fldr_param(tvb, pinfo, afp_tree, offset);break;
		case AFP_SETFLDRPARAM:
			offset = dissect_query_afp_set_fldr_param(tvb, pinfo, afp_tree, offset);break;
		case AFP_CHANGEPW:
		case AFP_GETSRVRMSG:
			break;
		case AFP_DELETE:	/* same as create_id */
		case AFP_CREATEDIR:
		case AFP_CREATEID:
			offset = dissect_query_afp_create_id(tvb, pinfo, afp_tree, offset);break; 
		case AFP_DELETEID:
			offset = dissect_query_afp_delete_id(tvb, pinfo, afp_tree, offset);break; 
		case AFP_RESOLVEID:
			offset = dissect_query_afp_resolve_id(tvb, pinfo, afp_tree, offset);break; 
		case AFP_EXCHANGEFILE:
		case AFP_CATSEARCH:
			break;
		case AFP_GETICON:
			offset = dissect_query_afp_get_icon(tvb, pinfo, afp_tree, offset);break; 
		case AFP_GTICNINFO:
			offset = dissect_query_afp_get_icon_info(tvb, pinfo, afp_tree, offset);break; 
		case AFP_ADDAPPL:
			offset = dissect_query_afp_add_appl(tvb, pinfo, afp_tree, offset);break; 
		case AFP_RMVAPPL:
			offset = dissect_query_afp_rmv_appl(tvb, pinfo, afp_tree, offset);break; 
		case AFP_GETAPPL:
			offset = dissect_query_afp_get_appl(tvb, pinfo, afp_tree, offset);break; 
		case AFP_ADDCMT:
			offset = dissect_query_afp_add_cmt(tvb, pinfo, afp_tree, offset);break; 
		case AFP_RMVCMT: /* same as get_cmt */
		case AFP_GETCMT:
			offset = dissect_query_afp_get_cmt(tvb, pinfo, afp_tree, offset);break; 
		case AFP_ADDICON:
			offset = dissect_query_afp_add_icon(tvb, pinfo, afp_tree, offset);break; 
			break;
 		}
	}
 	else {
		proto_tree_add_uint(afp_tree, hf_afp_command, tvb, 0, 0, afp_command);
 		switch(afp_command) {
		case AFP_BYTELOCK:
			offset = dissect_reply_afp_byte_lock(tvb, pinfo, afp_tree, offset);break; 
 		case AFP_ENUMERATE:
 			offset = dissect_reply_afp_enumerate(tvb, pinfo, afp_tree, offset);break;
 		case AFP_OPENVOL:
 			offset = dissect_reply_afp_open_vol(tvb, pinfo, afp_tree, offset);break;
		case AFP_OPENFORK:
			offset = dissect_reply_afp_open_fork(tvb, pinfo, afp_tree, offset);break;
		case AFP_RESOLVEID:
		case AFP_GETFORKPARAM:
			offset =dissect_reply_afp_get_fork_param(tvb, pinfo, afp_tree, offset);break;
		case AFP_GETSRVINFO:
		case AFP_GETSRVPARAM:
			break;
		case AFP_CREATEDIR:
			offset = dissect_reply_afp_create_dir(tvb, pinfo, afp_tree, offset);break; 
		case AFP_MOVE:		/* same as create_id */
		case AFP_CREATEID:
			offset = dissect_reply_afp_create_id(tvb, pinfo, afp_tree, offset);break; 
		case AFP_GETVOLPARAM:
			offset = dissect_reply_afp_get_vol_param(tvb, pinfo, afp_tree, offset);break;
 		case AFP_GETFLDRPARAM:
 			offset = dissect_reply_afp_get_fldr_param(tvb, pinfo, afp_tree, offset);break;
		case AFP_OPENDT:
			offset = dissect_reply_afp_open_dt(tvb, pinfo, afp_tree, offset);break;
		case AFP_GETICON:
			offset = dissect_reply_afp_get_icon(tvb, pinfo, afp_tree, offset);break; 
		case AFP_GTICNINFO:
			offset = dissect_reply_afp_get_icon_info(tvb, pinfo, afp_tree, offset);break; 
		case AFP_GETAPPL:
			offset = dissect_reply_afp_get_appl(tvb, pinfo, afp_tree, offset);break; 
		case AFP_GETCMT:
			offset = dissect_reply_afp_get_cmt(tvb, pinfo, afp_tree, offset);break; 
		case AFP_WRITE:
			offset = dissect_reply_afp_write(tvb, pinfo, afp_tree, offset);break;
		}
	}
	if (tree && offset < len)
		call_dissector(data_handle,tvb_new_subset(tvb, offset,-1,tvb_reported_length_remaining(tvb,offset)), pinfo, afp_tree);
}

static void afp_reinit( void)
{

	if (afp_request_hash)
		g_hash_table_destroy(afp_request_hash);
	if (afp_request_keys)
		g_mem_chunk_destroy(afp_request_keys);
	if (afp_request_vals)
		g_mem_chunk_destroy(afp_request_vals);

	afp_request_hash = g_hash_table_new(afp_hash, afp_equal);

	afp_request_keys = g_mem_chunk_new("afp_request_keys",
		sizeof(afp_request_key),
		afp_packet_init_count * sizeof(afp_request_key),
		G_ALLOC_AND_FREE);
	afp_request_vals = g_mem_chunk_new("afp_request_vals",
		sizeof(afp_request_val),
		afp_packet_init_count * sizeof(afp_request_val),
		G_ALLOC_AND_FREE);

}

void
proto_register_afp(void)
{

  static hf_register_info hf[] = {
    { &hf_afp_command,
      { "Command",      "afp.command",
		FT_UINT8, BASE_DEC, VALS(CommandCode_vals), 0x0,
      	"AFP function", HFILL }},

    { &hf_afp_pad,    
      { "Pad",    	"afp.pad",    
		FT_NONE,   BASE_NONE, NULL, 0, 
	"Pad Byte",	HFILL }},

    { &hf_afp_AFPVersion,
      { "AFP Version",  "afp.AFPVersion",
		FT_UINT_STRING, BASE_NONE, NULL, 0x0,
      	"Client AFP version", HFILL }},

    { &hf_afp_UAM,
      { "UAM",          "afp.UAM",
		FT_UINT_STRING, BASE_NONE, NULL, 0x0,
      	"User Authentication Method", HFILL }},

    { &hf_afp_user,
      { "User",         "afp.user",
		FT_UINT_STRING, BASE_NONE, NULL, 0x0,
      	"User", HFILL }},

    { &hf_afp_passwd,
      { "Password",     "afp.passwd",
		FT_STRINGZ, BASE_NONE, NULL, 0x0,
      	"Password", HFILL }},

    { &hf_afp_vol_bitmap,
      { "Bitmap",         "afp.vol_bitmap",
		FT_UINT16, BASE_HEX, NULL, 0 /* 0x0FFF*/,
      	"Volume bitmap", HFILL }},

    { &hf_afp_vol_bitmap_Attributes,
      { "Attributes",      "afp.vol_bitmap.attributes",
		FT_BOOLEAN, 16, NULL, kFPVolAttributeBit,
      	"Volume attributes", HFILL }},

    { &hf_afp_vol_attribute,
      { "Attributes",         "afp.vol_attributes",
		FT_UINT16, BASE_HEX, NULL, 0,
	"Volume attributes", HFILL }},

    { &hf_afp_vol_attribute_ReadOnly, 
      { "Read only",         "afp.vol_attribute.read_only",
		 FT_BOOLEAN, 16, NULL, kReadOnly,
        "Read only volume", HFILL }},

    { &hf_afp_vol_attribute_HasVolumePassword,
      { "Volume password",         "afp.vol_attribute.passwd",
		 FT_BOOLEAN, 16, NULL, kHasVolumePassword,
      	"Has a volume password", HFILL }},

    { &hf_afp_vol_attribute_SupportsFileIDs,
      { "File IDs",         "afp.vol_attribute.fileIDs",
		 FT_BOOLEAN, 16, NULL, kSupportsFileIDs,
	"Supports file IDs", HFILL }},

    { &hf_afp_vol_attribute_SupportsCatSearch,
      { "Catalog search",         "afp.vol_attribute.cat_search",
		 FT_BOOLEAN, 16, NULL, kSupportsCatSearch,
      	"Supports catalog search operations", HFILL }},

    { &hf_afp_vol_attribute_SupportsBlankAccessPrivs,
      { "Blank access privileges",         "afp.vol_attribute.blank_access_privs",
		 FT_BOOLEAN, 16, NULL, kSupportsBlankAccessPrivs,
        "Supports blank access privileges", HFILL }},

    { &hf_afp_vol_attribute_SupportsUnixPrivs,
      { "UNIX access privileges",         "afp.vol_attribute.unix_privs",
		 FT_BOOLEAN, 16, NULL, kSupportsUnixPrivs,
      	"Supports UNIX access privileges", HFILL }},

    { &hf_afp_vol_attribute_SupportsUTF8Names,
      { "UTF-8 names",         "afp.vol_attribute.utf8_names",
		 FT_BOOLEAN, 16, NULL, kSupportsUTF8Names,
      	"Supports UTF-8 names", HFILL }},

    { &hf_afp_vol_bitmap_Signature,
      { "Signature",         "afp.vol_bitmap.signature",
		FT_BOOLEAN, 16, NULL, kFPVolSignatureBit,
      	"Volume signature", HFILL }},

    { &hf_afp_vol_bitmap_CreateDate,
      { "Creation date",      "afp.vol_bitmap.create_date",
		FT_BOOLEAN, 16, NULL, kFPVolCreateDateBit,
      	"Volume creation date", HFILL }},

    { &hf_afp_vol_bitmap_ModDate,
      { "Modification date",  "afp.vol_bitmap.mod_date",
		FT_BOOLEAN, 16, NULL, kFPVolModDateBit,
      	"Volume modification date", HFILL }},

    { &hf_afp_vol_bitmap_BackupDate,
      { "Backup date",        "afp.vol_bitmap.backup_date",
		FT_BOOLEAN, 16, NULL, kFPVolBackupDateBit,
      	"Volume backup date", HFILL }},

    { &hf_afp_vol_bitmap_ID,
      { "ID",         "afp.vol_bitmap.id",
		FT_BOOLEAN, 16, NULL,  kFPVolIDBit,
      	"Volume ID", HFILL }},

    { &hf_afp_vol_bitmap_BytesFree,
      { "Bytes free",         "afp.vol_bitmap.bytes_free",
		FT_BOOLEAN, 16, NULL,  kFPVolBytesFreeBit,
      	"Volume free bytes", HFILL }},

    { &hf_afp_vol_bitmap_BytesTotal,
      { "Bytes total",         "afp.vol_bitmap.bytes_total",
		FT_BOOLEAN, 16, NULL,  kFPVolBytesTotalBit,
      	"Volume total bytes", HFILL }},

    { &hf_afp_vol_bitmap_Name,
      { "Name",         "afp.vol_bitmap.name",
		FT_BOOLEAN, 16, NULL,  kFPVolNameBit,
      	"Volume name", HFILL }},

    { &hf_afp_vol_bitmap_ExtBytesFree,
      { "Extended bytes free",         "afp.vol_bitmap.ex_bytes_free",
		FT_BOOLEAN, 16, NULL,  kFPVolExtBytesFreeBit,
      	"Volume extended (>2GB) free bytes", HFILL }},

    { &hf_afp_vol_bitmap_ExtBytesTotal,
      { "Extended bytes total",         "afp.vol_bitmap.ex_bytes_total",
		FT_BOOLEAN, 16, NULL,  kFPVolExtBytesTotalBit,
      	"Volume extended (>2GB) total bytes", HFILL }},

    { &hf_afp_vol_bitmap_BlockSize,
      { "Block size",         "afp.vol_bitmap.block_size",
		FT_BOOLEAN, 16, NULL,  kFPVolBlockSizeBit,
      	"Volume block size", HFILL }},

    { &hf_afp_dir_bitmap_Attributes,        
      { "Attributes",         "afp.dir_bitmap.attributes",
	    FT_BOOLEAN, 16, NULL,  kFPAttributeBit,
      	"Return attributes if directory", HFILL }},

    { &hf_afp_dir_bitmap_ParentDirID,	   
      { "DID",         "afp.dir_bitmap.did",
    	FT_BOOLEAN, 16, NULL,  kFPParentDirIDBit,
      	"Return parent directory ID if directory", HFILL }},

    { &hf_afp_dir_bitmap_CreateDate,	   
      { "Creation date",         "afp.dir_bitmap.create_date",
	    FT_BOOLEAN, 16, NULL,  kFPCreateDateBit,
      	"Return creation date if directory", HFILL }},

    { &hf_afp_dir_bitmap_ModDate,		   
      { "Modification date",         "afp.dir_bitmap.mod_date",
    	FT_BOOLEAN, 16, NULL,  kFPModDateBit,
      	"Return modification date if directory", HFILL }},

    { &hf_afp_dir_bitmap_BackupDate,	   
      { "Backup date",         "afp.dir_bitmap.backup_date",
	    FT_BOOLEAN, 16, NULL,  kFPBackupDateBit,
      	"Return backup date if directory", HFILL }},

    { &hf_afp_dir_bitmap_FinderInfo,	   
      { "Finder info",         "afp.dir_bitmap.finder_info",
    	FT_BOOLEAN, 16, NULL,  kFPFinderInfoBit,
      	"Return finder info if directory", HFILL }},

    { &hf_afp_dir_bitmap_LongName,		   
      { "Long name",         "afp.dir_bitmap.long_name",
	    FT_BOOLEAN, 16, NULL,  kFPLongNameBit,
      	"Return long name if directory", HFILL }},

    { &hf_afp_dir_bitmap_ShortName,		   
      { "Short name",         "afp.dir_bitmap.short_name",
    	FT_BOOLEAN, 16, NULL,  kFPShortNameBit,
      	"Return short name if directory", HFILL }},

    { &hf_afp_dir_bitmap_NodeID,		   
      { "File ID",         "afp.dir_bitmap.fid",
	    FT_BOOLEAN, 16, NULL,  kFPNodeIDBit,
      	"Return file ID if directory", HFILL }},

    { &hf_afp_dir_bitmap_OffspringCount,   
      { "Offspring count",         "afp.dir_bitmap.offspring_count",
    	FT_BOOLEAN, 16, NULL,  kFPOffspringCountBit,
      	"Return offspring count if directory", HFILL }},

    { &hf_afp_dir_bitmap_OwnerID,		   
      { "Owner id",         "afp.dir_bitmap.owner_id",
	    FT_BOOLEAN, 16, NULL,  kFPOwnerIDBit,
      	"Return owner id if directory", HFILL }},

    { &hf_afp_dir_bitmap_GroupID,		   
      { "Group id",         "afp.dir_bitmap.group_id",
    	FT_BOOLEAN, 16, NULL,  kFPGroupIDBit,
      	"Return group id if directory", HFILL }},

    { &hf_afp_dir_bitmap_AccessRights,	   
      { "Access rights",         "afp.dir_bitmap.access_rights",
	    FT_BOOLEAN, 16, NULL,  kFPAccessRightsBit,
      	"Return access rights if directory", HFILL }},

    { &hf_afp_dir_bitmap_UTF8Name,		   
      { "UTF-8 name",         "afp.dir_bitmap.UTF8_name",
    	FT_BOOLEAN, 16, NULL,  kFPUTF8NameBit,
      	"Return UTF-8 name if diectory", HFILL }},

    { &hf_afp_dir_bitmap_UnixPrivs,		   
      { "UNIX privileges",         "afp.dir_bitmap.unix_privs",
	    FT_BOOLEAN, 16, NULL,  kFPUnixPrivsBit,
      	"Return UNIX privileges if directory", HFILL }},

    { &hf_afp_dir_attribute_Invisible,
      { "Invisible",         "afp.dir_attribute.invisible",
	    FT_BOOLEAN, 16, NULL,  kFPInvisibleBit,
      	"Directory is not visible", HFILL }},

    { &hf_afp_dir_attribute_IsExpFolder,
      { "Share point",         "afp.dir_attribute.share",
	    FT_BOOLEAN, 16, NULL,  kFPMultiUserBit,
      	"Directory is a share point", HFILL }},

    { &hf_afp_dir_attribute_System,
      { "System",         	 "afp.dir_attribute.system",
	    FT_BOOLEAN, 16, NULL,  kFPSystemBit,
      	"Directory is a system directory", HFILL }},

    { &hf_afp_dir_attribute_Mounted,
      { "Mounted",         "afp.dir_attribute.mounted",
	    FT_BOOLEAN, 16, NULL,  kFPDAlreadyOpenBit,
      	"Directory is mounted", HFILL }},

    { &hf_afp_dir_attribute_InExpFolder,
      { "Shared area",         "afp.dir_attribute.in_exported_folder",
	    FT_BOOLEAN, 16, NULL,  kFPRAlreadyOpenBit,
      	"Directory is in a shared area", HFILL }},

    { &hf_afp_dir_attribute_BackUpNeeded,
      { "Backup needed",         "afp.dir_attribute.backup_needed",
	    FT_BOOLEAN, 16, NULL,  kFPBackUpNeededBit,
      	"Directory needs to be backed up", HFILL }},

    { &hf_afp_dir_attribute_RenameInhibit,
      { "Rename inhibit",         "afp.dir_attribute.rename_inhibit",
	    FT_BOOLEAN, 16, NULL,  kFPRenameInhibitBit,
      	"Rename inhibit", HFILL }},

    { &hf_afp_dir_attribute_DeleteInhibit,
      { "Delete inhibit",         "afp.dir_attribute.delete_inhibit",
	    FT_BOOLEAN, 16, NULL,  kFPDeleteInhibitBit,
      	"Delete inhibit", HFILL }},

    { &hf_afp_dir_attribute_SetClear,
      { "Set",         "afp.dir_attribute.set_clear",
	    FT_BOOLEAN, 16, NULL,  kFPSetClearBit,
      	"Clear/set attribute", HFILL }},

    { &hf_afp_file_bitmap_Attributes,
      { "Attributes",         "afp.file_bitmap.attributes",
	    FT_BOOLEAN, 16, NULL,  kFPAttributeBit,
      	"Return attributes if file", HFILL }},

    { &hf_afp_file_bitmap_ParentDirID,	   
      { "DID",         "afp.file_bitmap.did",
    	FT_BOOLEAN, 16, NULL,  kFPParentDirIDBit,
      	"Return parent directory ID if file", HFILL }},

    { &hf_afp_file_bitmap_CreateDate,	   
      { "Creation date",         "afp.file_bitmap.create_date",
	    FT_BOOLEAN, 16, NULL,  kFPCreateDateBit,
      	"Return creation date if file", HFILL }},

    { &hf_afp_file_bitmap_ModDate,		   
      { "Modification date",         "afp.file_bitmap.mod_date",
    	FT_BOOLEAN, 16, NULL,  kFPModDateBit,
      	"Return modification date if file", HFILL }},

    { &hf_afp_file_bitmap_BackupDate,	   
      { "Backup date",         "afp.file_bitmap.backup_date",
	    FT_BOOLEAN, 16, NULL,  kFPBackupDateBit,
      	"Return backup date if file", HFILL }},

    { &hf_afp_file_bitmap_FinderInfo,	   
      { "Finder info",         "afp.file_bitmap.finder_info",
    	FT_BOOLEAN, 16, NULL,  kFPFinderInfoBit,
      	"Return finder info if file", HFILL }},

    { &hf_afp_file_bitmap_LongName,		   
      { "Long name",         "afp.file_bitmap.long_name",
	    FT_BOOLEAN, 16, NULL,  kFPLongNameBit,
      	"Return long name if file", HFILL }},

    { &hf_afp_file_bitmap_ShortName,		   
      { "Short name",         "afp.file_bitmap.short_name",
    	FT_BOOLEAN, 16, NULL,  kFPShortNameBit,
      	"Return short name if file", HFILL }},

    { &hf_afp_file_bitmap_NodeID,		   
      { "File ID",         "afp.file_bitmap.fid",
	    FT_BOOLEAN, 16, NULL,  kFPNodeIDBit,
      	"Return file ID if file", HFILL }},

    { &hf_afp_file_bitmap_DataForkLen,
      { "Data fork size",         "afp.file_bitmap.data_fork_len",
	    FT_BOOLEAN, 16, NULL,  kFPDataForkLenBit,
      	"Return data fork size if file", HFILL }},

    { &hf_afp_file_bitmap_RsrcForkLen,
      { "Resource fork size",         "afp.file_bitmap.resource_fork_len",
	    FT_BOOLEAN, 16, NULL,  kFPRsrcForkLenBit,
      	"Return resource fork size if file", HFILL }},

    { &hf_afp_file_bitmap_ExtDataForkLen,
      { "Extended data fork size",         "afp.file_bitmap.ex_data_fork_len",
	    FT_BOOLEAN, 16, NULL,  kFPExtDataForkLenBit,
      	"Return extended (>2GB) data fork size if file", HFILL }},

    { &hf_afp_file_bitmap_LaunchLimit,
      { "Launch limit",         "afp.file_bitmap.launch_limit",
	    FT_BOOLEAN, 16, NULL,  kFPLaunchLimitBit,
      	"Return launch limit if file", HFILL }},

    { &hf_afp_file_bitmap_UTF8Name,		   
      { "UTF-8 name",         "afp.file_bitmap.UTF8_name",
    	FT_BOOLEAN, 16, NULL,  kFPUTF8NameBit,
      	"Return UTF-8 name if file", HFILL }},

    { &hf_afp_file_bitmap_ExtRsrcForkLen,
      	{ "Extended resource fork size",         "afp.file_bitmap.ex_resource_fork_len",
	    FT_BOOLEAN, 16, NULL,  kFPExtRsrcForkLenBit,
      	"Return extended (>2GB) resource fork size if file", HFILL }},

    { &hf_afp_file_bitmap_UnixPrivs,		   
      { "UNIX privileges",    "afp.file_bitmap.unix_privs",
	    FT_BOOLEAN, 16, NULL,  kFPUnixPrivsBit_file,
      	"Return UNIX privileges if file", HFILL }},

	/* ---------- */
    { &hf_afp_file_attribute_Invisible,
      { "Invisible",         "afp.file_attribute.invisible",
	    FT_BOOLEAN, 16, NULL,  kFPInvisibleBit,
      	"File is not visible", HFILL }},

    { &hf_afp_file_attribute_MultiUser,
      { "Multi user",         "afp.file_attribute.multi_user",
	    FT_BOOLEAN, 16, NULL,  kFPMultiUserBit,
      	"multi user", HFILL }},

    { &hf_afp_file_attribute_System,
      { "System",         	 "afp.file_attribute.system",
	    FT_BOOLEAN, 16, NULL,  kFPSystemBit,
      	"File is a system file", HFILL }},

    { &hf_afp_file_attribute_DAlreadyOpen,
      { "Data fork open",         "afp.file_attribute.df_open",
	    FT_BOOLEAN, 16, NULL,  kFPDAlreadyOpenBit,
      	"Data fork already open", HFILL }},

    { &hf_afp_file_attribute_RAlreadyOpen,
      { "Resource fork open",         "afp.file_attribute.rf_open",
	    FT_BOOLEAN, 16, NULL,  kFPRAlreadyOpenBit,
      	"Resource fork already open", HFILL }},

    { &hf_afp_file_attribute_WriteInhibit,
      { "Write inhibit",         "afp.file_attribute.write_inhibit",
	    FT_BOOLEAN, 16, NULL,  kFPWriteInhibitBit,
      	"Write inhibit", HFILL }},

    { &hf_afp_file_attribute_BackUpNeeded,
      { "Backup needed",         "afp.file_attribute.backup_needed",
	    FT_BOOLEAN, 16, NULL,  kFPBackUpNeededBit,
      	"File needs to be backed up", HFILL }},

    { &hf_afp_file_attribute_RenameInhibit,
      { "Rename inhibit",         "afp.file_attribute.rename_inhibit",
	    FT_BOOLEAN, 16, NULL,  kFPRenameInhibitBit,
      	"rename inhibit", HFILL }},

    { &hf_afp_file_attribute_DeleteInhibit,
      { "Delete inhibit",         "afp.file_attribute.delete_inhibit",
	    FT_BOOLEAN, 16, NULL,  kFPDeleteInhibitBit,
      	"delete inhibit", HFILL }},

    { &hf_afp_file_attribute_CopyProtect,
      { "Copy protect",         "afp.file_attribute.copy_protect",
	    FT_BOOLEAN, 16, NULL,  kFPCopyProtectBit,
      	"copy protect", HFILL }},

    { &hf_afp_file_attribute_SetClear,
      { "Set",         "afp.file_attribute.set_clear",
	    FT_BOOLEAN, 16, NULL,  kFPSetClearBit,
      	"Clear/set attribute", HFILL }},
	/* ---------- */

    { &hf_afp_vol_name,
      { "Volume",         "afp.vol_name",
	FT_UINT_STRING, BASE_NONE, NULL, 0x0,
      	"Volume name", HFILL }},

    { &hf_afp_vol_id,
      { "Volume id",         "afp.vol_id",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"Volume id", HFILL }},

    { &hf_afp_vol_signature,
      { "Signature",         "afp.vol_signature",
		FT_UINT16, BASE_DEC, VALS(vol_signature_vals), 0x0,
      	"Volume signature", HFILL }},

    { &hf_afp_bitmap_offset,
      { "Offset",         "afp.bitmap_offset",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"Name offset in packet", HFILL }},

    { &hf_afp_vol_creation_date,
      { "Creation date",         "afp.vol_creation_date",
		FT_ABSOLUTE_TIME, BASE_NONE, NULL, 0x0,
      	"Volume creation date", HFILL }},

    { &hf_afp_vol_modification_date,
      { "Modification date",         "afp.vol_modification_date",
		FT_ABSOLUTE_TIME, BASE_NONE, NULL, 0x0,
      	"Volume modification date", HFILL }},

    { &hf_afp_vol_backup_date,
      { "Backup date",         "afp.vol_backup_date",
		FT_ABSOLUTE_TIME, BASE_NONE, NULL, 0x0,
      	"Volume backup date", HFILL }},

    { &hf_afp_vol_bytes_free,
      { "Bytes free",         "afp.vol_bytes_free",
		FT_UINT32, BASE_DEC, NULL, 0x0,
      	"Free space", HFILL }},

    { &hf_afp_vol_bytes_total,
      { "Bytes total",         "afp.vol_bytes_total",
		FT_UINT32, BASE_DEC, NULL, 0x0,
      	"Volume size", HFILL }},

    { &hf_afp_vol_ex_bytes_free,
      { "Extended bytes free",         "afp.vol_ex_bytes_free",
		FT_UINT64, BASE_DEC, NULL, 0x0,
      	"Extended (>2GB) free space", HFILL }},

    { &hf_afp_vol_ex_bytes_total,
      { "Extended bytes total",         "afp.vol_ex_bytes_total",
		FT_UINT64, BASE_DEC, NULL, 0x0,
      	"Extended (>2GB) volume size", HFILL }},

    { &hf_afp_vol_block_size,
      { "Block size",         "afp.vol_block_size",
		FT_UINT32, BASE_DEC, NULL, 0x0,
      	"Volume block size", HFILL }},

    { &hf_afp_did,
      { "DID",         "afp.did",
		FT_UINT32, BASE_DEC, NULL, 0x0,
      	"Parent directory ID", HFILL }},

    { &hf_afp_dir_bitmap,
      { "Directory bitmap",         "afp.dir_bitmap",
		FT_UINT16, BASE_HEX, NULL, 0x0,
      	"Directory bitmap", HFILL }},

    { &hf_afp_dir_offspring,
      { "Offspring",         "afp.dir_offspring",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"Directory offspring", HFILL }},

    { &hf_afp_dir_OwnerID,
      { "Owner ID",         "afp.dir_owner_id",
		FT_INT32, BASE_DEC, NULL, 0x0,
      	"Directory owner ID", HFILL }},

    { &hf_afp_dir_GroupID,
      { "Group ID",         "afp.dir_group_id",
		FT_INT32, BASE_DEC, NULL, 0x0,
      	"Directory group ID", HFILL }},

    { &hf_afp_creation_date,
      { "Creation date",         "afp.creation_date",
		FT_ABSOLUTE_TIME, BASE_NONE, NULL, 0x0,
      	"Creation date", HFILL }},

    { &hf_afp_modification_date,
      { "Modification date",         "afp.modification_date",
		FT_ABSOLUTE_TIME, BASE_NONE, NULL, 0x0,
      	"Modification date", HFILL }},

    { &hf_afp_backup_date,
      { "Backup date",         "afp.backup_date",
		FT_ABSOLUTE_TIME, BASE_NONE, NULL, 0x0,
      	"Backup date", HFILL }},

    { &hf_afp_finder_info,
      { "Finder info",         "afp.finder_info",
		FT_BYTES, BASE_HEX, NULL, 0x0,
      	"Finder info", HFILL }},

    { &hf_afp_file_id,
      { "File ID",         "afp.file_id",
		FT_UINT32, BASE_DEC, NULL, 0x0,
      	"File/directory ID", HFILL }},

    { &hf_afp_file_DataForkLen,
      { "Data fork size",         "afp.data_fork_len",
		FT_UINT32, BASE_DEC, NULL, 0x0,
      	"Data fork size", HFILL }},

    { &hf_afp_file_RsrcForkLen,
      { "Resource fork size",         "afp.resource_fork_len",
		FT_UINT32, BASE_DEC, NULL, 0x0,
      	"Resource fork size", HFILL }},

    { &hf_afp_file_ExtDataForkLen,
      { "Extended data fork size",         "afp.ext_data_fork_len",
		FT_UINT64, BASE_DEC, NULL, 0x0,
      	"Extended (>2GB) data fork length", HFILL }},

    { &hf_afp_file_ExtRsrcForkLen,
      { "Extended resource fork size",         "afp.ext_resource_fork_len",
		FT_UINT64, BASE_DEC, NULL, 0x0,
      	"Extended (>2GB) resource fork length", HFILL }},

    { &hf_afp_file_UnixPrivs,
      { "UNIX privileges",         "afp.unix_privs",
		FT_UINT32, BASE_DEC, NULL, 0x0,
      	"UNIX privileges", HFILL }},
    
    { &hf_afp_file_bitmap,
      { "File bitmap",         "afp.file_bitmap",
		FT_UINT16, BASE_HEX, NULL, 0x0,
      	"File bitmap", HFILL }},
    
    { &hf_afp_req_count,
      { "Req count",         "afp.req_count",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"Maximum number of structures returned", HFILL }},

    { &hf_afp_start_index, 
      { "Start index",         "afp.start_index",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"First structure returned", HFILL }},
    
    { &hf_afp_max_reply_size,
      { "Reply size",         "afp.reply_size",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"First structure returned", HFILL }},

    { &hf_afp_file_flag,
      { "Dir",         "afp.flag",
		FT_BOOLEAN, 8, NULL, 0x80,
      	"Is a dir", HFILL }},

    { &hf_afp_create_flag,
      { "Hard create",         "afp.create_flag",
		FT_BOOLEAN, 8, NULL, 0x80,
      	"Soft/hard create file", HFILL }},

    { &hf_afp_struct_size,
      { "Struct size",         "afp.struct_size",
		FT_UINT8, BASE_DEC, NULL,0,
      	"Sizeof of struct", HFILL }},
    
    { &hf_afp_flag,
      { "From",         "afp.flag",
		FT_UINT8, BASE_HEX, VALS(flag_vals), 0x80,
      	"Offset is relative to start/end of the fork", HFILL }},

    { &hf_afp_dt_ref,
      { "DT ref",         "afp.dt_ref",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"Desktop database reference num", HFILL }},

    { &hf_afp_ofork,
      { "Fork",         "afp.ofork",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"Open fork reference number", HFILL }},

    { &hf_afp_offset,
      { "Offset",         "afp.offset",
		FT_INT32, BASE_DEC, NULL, 0x0,
      	"Offset", HFILL }},
    
    { &hf_afp_rw_count,
      { "Count",         "afp.rw_count",
		FT_INT32, BASE_DEC, NULL, 0x0,
      	"Number of bytes to be read/written", HFILL }},
    
    { &hf_afp_last_written,
      { "Last written",  "afp.last_written",
		FT_UINT32, BASE_DEC, NULL, 0x0,
      	"Offset of the last byte written", HFILL }},

    { &hf_afp_actual_count,
      { "Count",         "afp.actual_count",
		FT_INT32, BASE_DEC, NULL, 0x0,
      	"Number of bytes returned by read/write", HFILL }},
      
    { &hf_afp_ofork_len,
      { "New length",         "afp.ofork_len",
		FT_INT32, BASE_DEC, NULL, 0x0,
      	"New length", HFILL }},

    { &hf_afp_path_type,
      { "Type",         "afp.path_type",
		FT_UINT8, BASE_HEX, VALS(path_type_vals), 0,
      	"Type of names", HFILL }},

    { &hf_afp_path_len,
      { "Len",  "afp.path_len",
		FT_UINT8, BASE_DEC, NULL, 0x0,
      	"Path length", HFILL }},

    { &hf_afp_path_name,
      { "Name",  "afp.path_name",
		FT_STRING, BASE_NONE, NULL, 0x0,
      	"Path name", HFILL }},

    { &hf_afp_fork_type,
      { "Resource fork",         "afp.fork_type",
		FT_BOOLEAN, 8, NULL, 0x80,
      	"Data/resource fork", HFILL }},

    { &hf_afp_access_mode,
      { "Access mode",         "afp.access",
		FT_UINT8, BASE_HEX, NULL, 0x0,
      	"Fork access mode", HFILL }},

    { &hf_afp_access_read,
      { "Read",         "afp.access.read",
    	FT_BOOLEAN, 8, NULL,  1,
      	"Open for reading", HFILL }},

    { &hf_afp_access_write,
      { "Write",         "afp.access.write",
    	FT_BOOLEAN, 8, NULL,  2,
      	"Open for writing", HFILL }},

    { &hf_afp_access_deny_read,
      { "Deny read",         "afp.access.deny_read",
    	FT_BOOLEAN, 8, NULL,  0x10,
      	"Deny read", HFILL }},

    { &hf_afp_access_deny_write,
      { "Deny write",         "afp.access.deny_write",
    	FT_BOOLEAN, 8, NULL,  0x20,
      	"Deny write", HFILL }},

    { &hf_afp_comment,
      { "Comment",         "afp.comment",
		FT_UINT_STRING, BASE_NONE, NULL, 0x0,
      	"File/folder comment", HFILL }},

    { &hf_afp_file_creator,
      { "File creator",         "afp.file_creator",
		FT_STRING, BASE_NONE, NULL, 0x0,
      	"File creator", HFILL }},

    { &hf_afp_file_type,
      { "File type",         "afp.file_type",
		FT_STRING, BASE_NONE, NULL, 0x0,
      	"File type", HFILL }},

    { &hf_afp_icon_type,
      { "Icon type",         "afp.icon_type",
		FT_UINT8, BASE_HEX, NULL , 0,
      	"Icon type", HFILL }},

    { &hf_afp_icon_length,
      { "Size",         "afp.icon_length",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"Size for icon bitmap", HFILL }},

    { &hf_afp_icon_index,
      { "Index",         "afp.icon_index",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"Icon index in desktop database", HFILL }},

    { &hf_afp_icon_tag,
      { "Tag",         "afp.icon_tag",
		FT_UINT32, BASE_HEX, NULL, 0x0,
      	"Icon tag", HFILL }},

    { &hf_afp_appl_index,
      { "Index",         "afp.appl_index",
		FT_UINT16, BASE_DEC, NULL, 0x0,
      	"Application index", HFILL }},

    { &hf_afp_appl_tag,
      { "Tag",         "afp.appl_tag",
		FT_UINT32, BASE_HEX, NULL, 0x0,
      	"Application tag", HFILL }},
      	
    { &hf_afp_lock_op,
      { "unlock",         "afp.lock_op",
		FT_BOOLEAN, 8, NULL, 0x1,
      	"Lock/unlock op", HFILL }},

    { &hf_afp_lock_from,
      { "End",         "afp.lock_from",
		FT_BOOLEAN, 8, NULL, 0x80,
      	"Offset is relative to the end of the fork", HFILL }},
    
    { &hf_afp_lock_offset,
      { "Offset",         "afp.lock_offset",
		FT_INT32, BASE_DEC, NULL, 0x0,
      	"First byte to be locked", HFILL }},

    { &hf_afp_lock_len,
      { "Length",         "afp.lock_len",
		FT_INT32, BASE_DEC, NULL, 0x0,
      	"Number of bytes to be locked/unlocked", HFILL }},

    { &hf_afp_lock_range_start,
      { "Start",         "afp.lock_range_start",
		FT_INT32, BASE_DEC, NULL, 0x0,
      	"First byte locked/unlocked", HFILL }},

    { &hf_afp_dir_ar,
      { "Access rights",         "afp.dir_ar",
		FT_UINT32, BASE_HEX, NULL, 0x0,
      	"Directory access rights", HFILL }},

    { &hf_afp_dir_ar_o_search,
      { "Owner has search access",      "afp.dir_ar.o_search",
		FT_BOOLEAN, 32, NULL, AR_O_SEARCH,
      	"Owner has search access", HFILL }},

    { &hf_afp_dir_ar_o_read,
      { "Owner has read access",        "afp.dir_ar.o_read",
		FT_BOOLEAN, 32, NULL, AR_O_READ,
      	"Owner has read access", HFILL }},

    { &hf_afp_dir_ar_o_write,
      { "Owner has write access",       "afp.dir_ar.o_write",
		FT_BOOLEAN, 32, NULL, AR_O_WRITE,
      	"Gwner has write access", HFILL }},

    { &hf_afp_dir_ar_g_search,
      { "Group has search access",      "afp.dir_ar.g_search",
		FT_BOOLEAN, 32, NULL, AR_G_SEARCH,
      	"Group has search access", HFILL }},

    { &hf_afp_dir_ar_g_read,
      { "Group has read access",        "afp.dir_ar.g_read",
		FT_BOOLEAN, 32, NULL, AR_G_READ,
      	"Group has read access", HFILL }},

    { &hf_afp_dir_ar_g_write,
      { "Group has write access",       "afp.dir_ar.g_write",
		FT_BOOLEAN, 32, NULL, AR_G_WRITE,
      	"Group has write access", HFILL }},

    { &hf_afp_dir_ar_e_search,
      { "Everyone has search access",   "afp.dir_ar.e_search",
		FT_BOOLEAN, 32, NULL, AR_E_SEARCH,
      	"Everyone has search access", HFILL }},

    { &hf_afp_dir_ar_e_read,
      { "Everyone has read access",     "afp.dir_ar.e_read",
		FT_BOOLEAN, 32, NULL, AR_E_READ,
      	"Everyone has read access", HFILL }},

    { &hf_afp_dir_ar_e_write,
      { "Everyone has write access",    "afp.dir_ar.e_write",
		FT_BOOLEAN, 32, NULL, AR_E_WRITE,
      	"Everyone has write access", HFILL }},

    { &hf_afp_dir_ar_u_search,
      { "User has search access",   "afp.dir_ar.u_search",
		FT_BOOLEAN, 32, NULL, AR_U_SEARCH,
      	"User has search access", HFILL }},

    { &hf_afp_dir_ar_u_read,
      { "User has read access",     "afp.dir_ar.u_read",
		FT_BOOLEAN, 32, NULL, AR_U_READ,
      	"User has read access", HFILL }},

    { &hf_afp_dir_ar_u_write,
      { "User has write access",     "afp.dir_ar.u_write",
		FT_BOOLEAN, 32, NULL, AR_U_WRITE,
      	"User has write access", HFILL }},

    { &hf_afp_dir_ar_blank,
      { "Blank access right",     "afp.dir_ar.blank",
		FT_BOOLEAN, 32, NULL, AR_BLANK,
      	"Blank access right", HFILL }},

    { &hf_afp_dir_ar_u_own,
      { "User is the owner",     "afp.dir_ar.u_owner",
		FT_BOOLEAN, 32, NULL, AR_U_OWN,
      	"Current user is the directory owner", HFILL }},
  };

  static gint *ett[] = {
	&ett_afp,
	&ett_afp_vol_bitmap,
	&ett_afp_vol_attribute,
	&ett_afp_dir_bitmap,
	&ett_afp_file_bitmap,
	&ett_afp_enumerate,
	&ett_afp_enumerate_line,
	&ett_afp_access_mode,
	&ett_afp_dir_attribute,
	&ett_afp_file_attribute,
	&ett_afp_path_name,
	&ett_afp_lock_flags,
	&ett_afp_dir_ar,
  };

  proto_afp = proto_register_protocol("AppleTalk Filing Protocol", "AFP", "afp");
  proto_register_field_array(proto_afp, hf, array_length(hf));
  proto_register_subtree_array(ett, array_length(ett));

  register_init_routine( &afp_reinit);

  register_dissector("afp", dissect_afp, proto_afp);
  data_handle = find_dissector("data");
}

void
proto_reg_handoff_afp(void)
{
  data_handle = find_dissector("data");
}

/* -------------------------------
   end
*/
