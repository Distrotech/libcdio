/*
    $Id: udf_fs.c,v 1.2 2005/10/24 03:12:30 rocky Exp $

    Copyright (C) 2005 Rocky Bernstein <rocky@panix.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
/*
 * Portions copyright (c) 2001, 2002 Scott Long <scottl@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_STRING_H
# include <string.h>
#endif

#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif

/* These definitions are also to make debugging easy. Note that they
   have to come *before* #include <cdio/ecma_167.h> which sets 
   #defines for these.
*/
const char VSD_STD_ID_BEA01[] = {'B', 'E', 'A', '0', '1'};
const char VSD_STD_ID_BOOT2[] = {'B', 'O', 'O', 'T', '2'};
const char VSD_STD_ID_CD001[] = {'C', 'D', '0', '0', '1'};
const char VSD_STD_ID_CDW01[] = {'C', 'D', 'W', '0', '2'};
const char VSD_STD_ID_NSR03[] = {'N', 'S', 'R', '0', '3'};
const char VSD_STD_ID_TEA01[] = {'T', 'E', 'A', '0', '1'};

#include <cdio/udf.h>
#include <cdio/ecma_167.h>
#include <cdio/bytesex.h>

/** The below variables are trickery to force enum symbol values to be
    recorded in debug symbol tables. They are used to allow one to refer
    to the enumeration value names in the typedefs above in a debugger
    and debugger expressions
*/
tag_id_t debug_tagid;
file_characteristics_t debug_file_characteristics;
udf_enum1_t debug_udf_enums1;

/* Private headers */
#include "_cdio_stdio.h"

/*
 * The UDF specs are pretty clear on how each data structure is made
 * up, but not very clear on how they relate to each other.  Here is
 * the skinny... This demostrates a filesystem with one file in the
 * root directory.  Subdirectories are treated just as normal files,
 * but they have File Id Descriptors of their children as their file
 * data.  As for the Anchor Volume Descriptor Pointer, it can exist in
 * two of the following three places: sector 256, sector n (the max
 * sector of the disk), or sector n - 256.  It's a pretty good bet
 * that one will exist at sector 256 though.  One caveat is unclosed
 * CD media.  For that, sector 256 cannot be written, so the Anchor
 * Volume Descriptor Pointer can exist at sector 512 until the media
 * is closed.
 *
 *  Sector:
 *     256:
 *       n: Anchor Volume Descriptor Pointer
 * n - 256:	|
 *		|
 *		|-->Main Volume Descriptor Sequence
 *			|	|
 *			|	|
 *			|	|-->Logical Volume Descriptor
 *			|			  |
 *			|-->Partition Descriptor  |
 *				|		  |
 *				|		  |
 *				|-->Fileset Descriptor
 *					|
 *					|
 *					|-->Root Dir File Entry
 *						|
 *						|
 *						|-->File data:
 *						    File Id Descriptor
 *							|
 *							|
 *							|-->File Entry
 *								|
 *								|
 *								|-->File data
 */

/* Implementation of udf_t type */
struct udf_s {
  bool          b_stream;         /* Use stream pointer, else use 
				    p_cdio.
				  */
  CdioDataSource_t      *stream;  /* Stream pointer if stream */
  CdIo_t                *cdio;    /* Cdio pointer if read device */
  anchor_vol_desc_ptr_t anchor_vol_desc_ptr;
  uint32_t              pvd_lba;  /* sector of Primary Volume Descriptor */
  partition_num_t       i_partition;  /* partition number */
  uint32_t              i_part_start; /* start of Partition Descriptor */
  uint32_t              lvd_lba;      /* sector of Logical Volume Descriptor */
  uint32_t              fsd_offset;   /* lba of fileset descriptor */
};

/**
 * Check the descriptor tag for both the correct id and correct checksum.
 * Return zero if all is good, -1 if not.
 */
static int
udf_checktag(udf_tag_t *p_tag, udf_Uint16_t tag_id)
{
  uint8_t *itag;
  uint8_t i;
  uint8_t cksum = 0;
  
  itag = (uint8_t *)p_tag;
  
  if (p_tag->id != tag_id)
    return -1;
  
  for (i = 0; i < 15; i++)
    cksum = cksum + itag[i];
  cksum = cksum - itag[4];
  
  if (cksum == p_tag->cksum)
    return 0;
  
  return -1;
}

static bool 
udf_get_lba(const udf_file_entry_t *p_fe, 
	    /*out*/ uint32_t *start, /*out*/ uint32_t *end)
{
  if (! p_fe->i_alloc_descs)
    return false;

  switch (p_fe->icb_tag.flags & ICBTAG_FLAG_AD_MASK) {
  case ICBTAG_FLAG_AD_SHORT:
    {
      udf_short_ad_t *p_ad = (udf_short_ad_t *) 
	(p_fe->ext_attr + p_fe->i_extended_attr);
      
      *start = uint32_from_le(p_ad->pos);
      *end = *start + 
	((uint32_from_le(p_ad->len) & UDF_LENGTH_MASK) - 1) / UDF_BLOCKSIZE;
      return true;
    }
    break;
  case ICBTAG_FLAG_AD_LONG:
    {
      udf_long_ad_t *p_ad = (udf_long_ad_t *) 
	(p_fe->ext_attr + p_fe->i_extended_attr);
      
      *start = uint32_from_le(p_ad->loc.lba); /* ignore partition number */
      *end = *start + 
	((uint32_from_le(p_ad->len) & UDF_LENGTH_MASK) - 1) / UDF_BLOCKSIZE;
      return true;
    }
    break;
  case ICBTAG_FLAG_AD_EXTENDED:
    {
      udf_ext_ad_t *p_ad = (udf_ext_ad_t *)
	(p_fe->ext_attr + p_fe->i_extended_attr);
      
      *start = uint32_from_le(p_ad->ext_loc.lba); /* ignore partition number */
      *end = *start + 
	((uint32_from_le(p_ad->len) & UDF_LENGTH_MASK) - 1) / UDF_BLOCKSIZE;
      return true;
    }
    break;
  default:
    return false;
  }
  return false;
}

#define udf_PATH_DELIMITERS "/\\"

static 
udf_file_t *
udf_ff_traverse(udf_t *p_udf, udf_file_t *p_udf_file, char *psz_token)
{
  while (udf_get_next(p_udf, p_udf_file)) {
    if (strcmp(psz_token, p_udf_file->psz_name) == 0) {
      char *next_tok = strtok(NULL, udf_PATH_DELIMITERS);
      
      if (!next_tok)
	return p_udf_file; /* found */
      else if (p_udf_file->b_dir) {
	udf_file_t * p_udf_file2 = udf_get_sub(p_udf, p_udf_file);
	
	if (p_udf_file2) {
	  udf_file_t * p_udf_file3 = 
	    udf_ff_traverse(p_udf, p_udf_file2, next_tok);
	  
	  if (!p_udf_file3) udf_file_free(p_udf_file2);
	  return p_udf_file3;
	}
      }
    }
  }
  return NULL;
}

/* FIXME! */
#define udf_MAX_PATHLEN 2048

udf_file_t * 
udf_find_file(udf_t *p_udf, const char *psz_name, const bool b_any_partition,
	      const partition_num_t i_partition)
{
  udf_file_t *p_udf_file = udf_get_root(p_udf, b_any_partition, i_partition);
  udf_file_t *p_udf_file2 = NULL;
  
  if (p_udf_file) {
    char tokenline[udf_MAX_PATHLEN];
    char *psz_token;
    
    strcpy(tokenline, psz_name);
    psz_token = strtok(tokenline, udf_PATH_DELIMITERS);
    if (psz_token)
      p_udf_file2 = udf_ff_traverse(p_udf, p_udf_file, psz_token);
    udf_file_free(p_udf_file);
  }
  return p_udf_file2;
}

/* Convert unicode16 to 8-bit char by dripping MSB. 
   Wonder if iconv can be used here
*/
static int 
unicode16_decode( uint8_t *data, int len, char *target ) 
{
    int p = 1, i = 0;

    if( ( data[ 0 ] == 8 ) || ( data[ 0 ] == 16 ) ) do {
        if( data[ 0 ] == 16 ) p++;  /* Ignore MSB of unicode16 */
        if( p < len ) {
            target[ i++ ] = data[ p++ ];
        }
    } while( p < len );

    target[ i ] = '\0';
    return 0;
}


static udf_file_t *
udf_new_file(udf_file_entry_t *p_fe, uint32_t i_part_start, 
	     const char *psz_name, bool b_dir, bool b_parent) 
{
  udf_file_t *p_udf_file = (udf_file_t *) calloc(1, sizeof(udf_file_t));
  if (!p_udf_file) return NULL;
  p_udf_file->psz_name     = strdup(psz_name);
  p_udf_file->b_dir        = b_dir;
  p_udf_file->b_parent     = b_parent;
  p_udf_file->i_part_start = i_part_start;
  p_udf_file->dir_left     = uint64_from_le(p_fe->info_len); 

  udf_get_lba( p_fe, &(p_udf_file->dir_lba), &(p_udf_file->dir_end_lba) );
  return p_udf_file;
}

/*!
  Seek to a position i_start and then read i_blocks. Number of blocks read is 
  returned. One normally expects the return to be equal to i_blocks.
*/
long int 
udf_read_sectors (const udf_t *p_udf, void *ptr, lsn_t i_start, 
		 long int i_blocks) 
{
  long int ret;
  long int i_byte_offset;
  
  if (!p_udf) return 0;
  i_byte_offset = (i_start * UDF_BLOCKSIZE);

  if (p_udf->b_stream) {
    ret = cdio_stream_seek (p_udf->stream, i_byte_offset, SEEK_SET);
    if (ret!=0) return 0;
    return cdio_stream_read (p_udf->stream, ptr, UDF_BLOCKSIZE, i_blocks);
  } else {
    return cdio_read_data_sectors(p_udf->cdio, ptr, i_start, UDF_BLOCKSIZE,
				  i_blocks);
  }
}

/*!
  Open an UDF for reading. Maybe in the future we will have
  a mode. NULL is returned on error.

  Caller must free result - use udf_close for that.
*/
udf_t *
udf_open (const char *psz_path)
{
  udf_t *p_udf = (udf_t *) calloc(1, sizeof(udf_t)) ;
  uint8_t data[UDF_BLOCKSIZE];

  if (!p_udf) return NULL;

  p_udf->b_stream = !cdio_is_device(psz_path, DRIVER_UNKNOWN);
  if (p_udf->b_stream) {
    p_udf->stream = cdio_stdio_new( psz_path );
    if (!p_udf->stream) 
      goto error;
  } else {
    p_udf->cdio = cdio_open(psz_path, DRIVER_UNKNOWN);
    if (!p_udf->cdio)
      goto error;
  }

  /*
   * Look for an Anchor Volume Descriptor Pointer at sector 256.
   */
  if (! udf_read_sectors (p_udf, &data, 256, 1) )
    goto error;
  
  memcpy(&(p_udf->anchor_vol_desc_ptr), &data, sizeof(anchor_vol_desc_ptr_t));

  if (udf_checktag((udf_tag_t *)&(p_udf->anchor_vol_desc_ptr), TAGID_ANCHOR))
    goto error;
  
  /*
   * Then try to find a reference to a Primary Volume Descriptor.
   */
  {
    const anchor_vol_desc_ptr_t *p_avdp = &p_udf->anchor_vol_desc_ptr;
    const uint32_t mvds_start = 
      uint32_from_le(p_avdp->main_vol_desc_seq_ext.loc);
    const uint32_t mvds_end   = mvds_start + 
      (uint32_from_le(p_avdp->main_vol_desc_seq_ext.len) - 1) / UDF_BLOCKSIZE;

    uint32_t i_lba;

    for (i_lba = mvds_start; i_lba < mvds_end; i_lba++) {

      udf_pvd_t *p_pvd = (udf_pvd_t *) &data;
      
      if (! udf_read_sectors (p_udf, p_pvd, i_lba, 1) ) 
	goto error;

      if (!udf_checktag(&p_pvd->tag, TAGID_PRI_VOL)) {
	p_udf->pvd_lba = i_lba;
	break;
      }
      
    }

    /*
     * If we couldn't find a reference, bail out.
     */
    if (i_lba == mvds_end)
      goto error;
  }

  return p_udf;

 error:
  free(p_udf);
  return NULL;
}

/*!
  Get the root in p_udf. If b_any_partition is false then
  the root must be in the given partition.
  NULL is returned if the partition is not found or a root is not found or
  there is on error.

  Caller must free result - use udf_file_free for that.
*/
udf_file_t *
udf_get_root (udf_t *p_udf, const bool b_any_partition,
	      const partition_num_t i_partition)
{
  const anchor_vol_desc_ptr_t *p_avdp = &p_udf->anchor_vol_desc_ptr;
  const uint32_t mvds_start = 
    uint32_from_le(p_avdp->main_vol_desc_seq_ext.loc);
  const uint32_t mvds_end   = mvds_start + 
    (uint32_from_le(p_avdp->main_vol_desc_seq_ext.len) - 1) / UDF_BLOCKSIZE;
  uint32_t i_lba;
  uint8_t data[UDF_BLOCKSIZE];

  /* 
     Now we have the joy of finding the Partition Descriptor and the
     Logical Volume Descriptor for the Main Volume Descriptor
     Sequence. Once we've got that, we use the Logical Volume
     Descriptor to get a Fileset Descriptor and that has the Root
     Directory File Entry.
  */
  for (i_lba = mvds_start; i_lba < mvds_end; i_lba++) {
    uint8_t data[UDF_BLOCKSIZE];
    
    partition_desc_t *p_partition = (partition_desc_t *) &data;
    
    if (! udf_read_sectors (p_udf, p_partition, i_lba, 1) ) 
      return NULL;
    
    if (!udf_checktag(&p_partition->tag, TAGID_PARTITION)) {
      const partition_num_t i_partition_check 
	= uint16_from_le(p_partition->number);
      if (b_any_partition || i_partition_check == i_partition) {
	/* Squirrel away some data regarding partition */
	p_udf->i_partition = uint16_from_le(p_partition->number);
	p_udf->i_part_start = uint32_from_le(p_partition->start_loc);
	if (p_udf->lvd_lba) break;
      }
    } else if (!udf_checktag(&p_partition->tag, TAGID_LOGVOL)) {
      /* Get fileset descriptor */
      logical_vol_desc_t *p_logvol = (logical_vol_desc_t *) &data;
      bool b_valid = 
	UDF_BLOCKSIZE == uint32_from_le(p_logvol->logical_blocksize);
      
      if (b_valid) {
	p_udf->lvd_lba = i_lba;
	p_udf->fsd_offset = 
	  uint32_from_le(p_logvol->lvd_use.fsd_loc.loc.lba);
	if (p_udf->i_part_start) break;
      }
    } 
  }
  if (p_udf->lvd_lba && p_udf->i_part_start) {
    udf_fsd_t *p_fsd = (udf_fsd_t *) &data;
    
    int i_sectors = udf_read_sectors(p_udf, p_fsd, 
				     p_udf->i_part_start + p_udf->fsd_offset,
				     1);
    
    if (i_sectors > 0 && !udf_checktag(&p_fsd->tag, TAGID_FSD)) {
      udf_file_entry_t *p_fe = (udf_file_entry_t *) &data;
      const uint32_t parent_icb = uint32_from_le(p_fsd->root_icb.loc.lba);
      
      /* Check partition numbers match of last-read block?  */
      
      udf_read_sectors(p_udf, p_fe, p_udf->i_part_start + parent_icb, 1);
      if (!udf_checktag(&p_fe->tag, TAGID_FILE_ENTRY)) {
	
	/* Check partition numbers match of last-read block? */
	
	/* We win! - Save root directory information. */
	return udf_new_file(p_fe, p_udf->i_part_start, "/", true, false );
      }
    }
  }

  return NULL;
}

/*!
  Close UDF and free resources associated with p_udf.
*/
bool 
udf_close (udf_t *p_udf) 
{
  if (!p_udf) return true;
  if (p_udf->b_stream) {
    cdio_stdio_destroy(p_udf->stream);
  } else {
    cdio_destroy(p_udf->cdio);
  }

  /* Get rid of root directory if allocated. */

  free(p_udf);
  return true;
}

udf_file_t * 
udf_get_sub(udf_t *p_udf, udf_file_t *p_udf_file)
{
  if (p_udf_file->b_dir && !p_udf_file->b_parent && p_udf_file->fid) {
    uint8_t data[UDF_BLOCKSIZE];
    udf_file_entry_t *p_fe = (udf_file_entry_t *) &data;
    
    int i_sectors = udf_read_sectors(p_udf, p_fe, p_udf->i_part_start 
				     + p_udf_file->fid->icb.loc.lba, 1);

    if (i_sectors && !udf_checktag(&p_fe->tag, TAGID_FILE_ENTRY)) {
      
      if (ICBTAG_FILE_TYPE_DIRECTORY == p_fe->icb_tag.file_type) {
	udf_file_t *p_udf_file_new = udf_new_file(p_fe, p_udf->i_part_start, 
						  p_udf_file->psz_name, 
						  true, true);
	return p_udf_file_new;
      }
    }
  }
  return NULL;
}

udf_file_t *
udf_get_next(udf_t *p_udf, udf_file_t *p_udf_file)
{

  if (p_udf_file->dir_left <= 0) {
    p_udf_file->fid = NULL;
    return NULL;
  }
  
  if (p_udf_file->fid) { 
    /* advance to next File Identifier Descriptor */
    uint32_t ofs = 4 * 
      ((sizeof(*(p_udf_file->fid)) + p_udf_file->fid->i_imp_use 
	+ p_udf_file->fid->i_file_id + 3) / 4);
    
    p_udf_file->fid = (udf_fileid_desc_t *)((uint8_t *)p_udf_file->fid + ofs);
  }
  
  if (!p_udf_file->fid) {
    uint32_t i_sectors = (p_udf_file->dir_end_lba - p_udf_file->dir_lba + 1);
    uint32_t size = UDF_BLOCKSIZE * i_sectors;
    int      i_read;

    if (!p_udf_file->sector)
      p_udf_file->sector = (uint8_t*) malloc(size);
    i_read = udf_read_sectors(p_udf, p_udf_file->sector, 
			      p_udf_file->i_part_start + p_udf_file->dir_lba, 
			      i_sectors);
    if (i_read)
      p_udf_file->fid = (udf_fileid_desc_t *) p_udf_file->sector;
    else
      p_udf_file->fid = NULL;
  }
  
  if (p_udf_file->fid && !udf_checktag(&(p_udf_file->fid->tag), TAGID_FID))
    {
      uint32_t ofs = 
	4 * ((sizeof(*p_udf_file->fid) + p_udf_file->fid->i_imp_use 
	      + p_udf_file->fid->i_file_id + 3) / 4);
      
      p_udf_file->dir_left -= ofs;
      p_udf_file->b_dir = 
	(p_udf_file->fid->file_characteristics & UDF_FILE_DIRECTORY) != 0;
      p_udf_file->b_parent = 
	(p_udf_file->fid->file_characteristics & UDF_FILE_PARENT) != 0;
      unicode16_decode(p_udf_file->fid->imp_use + p_udf_file->fid->i_imp_use, 
		       p_udf_file->fid->i_file_id, p_udf_file->psz_name);
      return p_udf_file;
    }
  return NULL;
}

/*!
  free free resources associated with p_fe.
*/
bool 
udf_file_free(udf_file_t *p_udf_file) 
{
  if (p_udf_file) {
    free(p_udf_file->psz_name);
    free(p_udf_file->sector);
    free(p_udf_file);
  }
  return true;
}