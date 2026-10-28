/*
 * Copyright 1997 Victor Schneider
 * Copyright 2002 Alexandre Julliard
 * Copyright 2007 Hans Leidekker
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <lzexpand.h>
#include <setupapi.h>

static int WINAPIV myprintf(const char* format, ...)
{
    va_list va;
    char        tmp[8192];
    DWORD       w = 0;
    int         len;

    va_start(va, format);
    len = vsnprintf(tmp, sizeof(tmp), format, va);
    if (len > 0)
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), tmp, len, &w, NULL);
    va_end(va);
    return w;
}

static UINT CALLBACK set_outfile( PVOID context, UINT notification, UINT_PTR param1, UINT_PTR param2 )
{
    FILE_IN_CABINET_INFO_A *info = (FILE_IN_CABINET_INFO_A *)param1;
    char buffer[MAX_PATH];
    char* basename;

    switch (notification)
    {
    case SPFILENOTIFY_FILEINCABINET:
    {
        LPSTR outfile = context;
        if (outfile[0] != 0)
        {
            SetLastError( ERROR_NOT_SUPPORTED );
            return FILEOP_ABORT;
        }
        GetFullPathNameA( info->NameInCabinet, sizeof(buffer), buffer, &basename );
        strcpy( outfile, basename );
        return FILEOP_SKIP;
    }
    default: return NO_ERROR;
    }
}

static UINT CALLBACK extract_callback( PVOID context, UINT notification, UINT_PTR param1, UINT_PTR param2 )
{
    FILE_IN_CABINET_INFO_A *info = (FILE_IN_CABINET_INFO_A *)param1;

    switch (notification)
    {
    case SPFILENOTIFY_FILEINCABINET:
    {
        LPCSTR targetname = context;

        strcpy( info->FullTargetName, targetname );
        return FILEOP_DOIT;
    }
    default: return NO_ERROR;
    }
}

static BOOL option_equal(LPCSTR str1, LPCSTR str2)
{
    if (str1[0] != '/' && str1[0] != '-')
        return FALSE;
    return !lstrcmpA( str1 + 1, str2 );
}

static BOOL output_dir_exists(LPCSTR outfile)
{
    char dir[MAX_PATH];
    char *basename;
    DWORD attr;

    if (!GetFullPathNameA( outfile, sizeof(dir), dir, &basename ))
        return TRUE;
    if (basename == dir)
        return TRUE;
    *basename = 0;
    attr = GetFileAttributesA( dir );
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        SetLastError( ERROR_PATH_NOT_FOUND );
        return FALSE;
    }
    return TRUE;
}

static LPCSTR filespec_from_arg(LPCSTR arg)
{
    if (arg[0] != '/' && arg[0] != '-')
        return NULL;
    if ((arg[1] == 'F' || arg[1] == 'f') && arg[2] == ':')
        return arg + 3;
    return NULL;
}

static BOOL match_filespec(LPCSTR name, LPCSTR pattern)
{
    while (*pattern)
    {
        if (*pattern == '*')
        {
            pattern++;
            if (!*pattern)
                return TRUE;
            while (*name)
            {
                if (match_filespec( name, pattern ))
                    return TRUE;
                name++;
            }
            return FALSE;
        }
        if (*pattern == '?')
        {
            if (!*name)
                return FALSE;
            name++;
            pattern++;
            continue;
        }
        if (!*name || tolower( (unsigned char)*name ) != tolower( (unsigned char)*pattern ))
            return FALSE;
        name++;
        pattern++;
    }
    return *name == 0;
}

static void join_path(LPSTR target, LPCSTR dir, LPCSTR name)
{
    size_t len = strlen( dir );

    strcpy( target, dir );
    if (len && dir[len - 1] != '\\' && dir[len - 1] != '/')
        strcat( target, "\\" );
    strcat( target, name );
}

static void make_parent_dirs(LPSTR path, size_t root_len)
{
    LPSTR p = path + root_len;

    if (*p == '\\' || *p == '/')
        p++;
    for (; *p; p++)
    {
        if (*p == '\\' || *p == '/')
        {
            char save = *p;
            *p = 0;
            CreateDirectoryA( path, NULL );
            *p = save;
        }
    }
}

static UINT CALLBACK count_callback( PVOID context, UINT notification, UINT_PTR param1, UINT_PTR param2 )
{
    if (notification == SPFILENOTIFY_FILEINCABINET)
    {
        (*(int *)context)++;
        return FILEOP_SKIP;
    }
    return NO_ERROR;
}

typedef struct
{
    LPCSTR pattern;
    LPCSTR destdir;
    LPCSTR fallback;
    int matches;
} fextract_ctx;

static UINT CALLBACK match_count_callback( PVOID context, UINT notification, UINT_PTR param1, UINT_PTR param2 )
{
    fextract_ctx *ctx = context;
    FILE_IN_CABINET_INFO_A *info = (FILE_IN_CABINET_INFO_A *)param1;

    if (notification == SPFILENOTIFY_FILEINCABINET)
    {
        if (match_filespec( info->NameInCabinet, ctx->pattern ))
            ctx->matches++;
        return FILEOP_SKIP;
    }
    return NO_ERROR;
}

static UINT CALLBACK fextract_callback( PVOID context, UINT notification, UINT_PTR param1, UINT_PTR param2 )
{
    fextract_ctx *ctx = context;
    FILE_IN_CABINET_INFO_A *info = (FILE_IN_CABINET_INFO_A *)param1;

    if (notification == SPFILENOTIFY_FILEINCABINET)
    {
        char target[MAX_PATH];

        if (!match_filespec( info->NameInCabinet, ctx->pattern ))
            return FILEOP_SKIP;
        join_path( target, ctx->destdir, ctx->fallback ? ctx->fallback : info->NameInCabinet );
        make_parent_dirs( target, strlen( ctx->destdir ) );
        myprintf( "Adding %s to extraction queue\n", target );
        strcpy( info->FullTargetName, target );
        return FILEOP_DOIT;
    }
    return NO_ERROR;
}

static int expand_cab_filespec( LPCSTR prog, LPCSTR infile, LPCSTR filespec, LPCSTR destdir )
{
    fextract_ctx ctx;
    char cabpath[MAX_PATH];
    char destpath[MAX_PATH];
    char *cabname;
    int members = 0;
    DWORD attr;

    GetFullPathNameA( destdir, sizeof(destpath), destpath, NULL );

    if (!SetupIterateCabinetA( infile, 0, count_callback, &members ))
    {
        myprintf( "%s: cabinet extraction failed\n", prog );
        return 1;
    }

    attr = GetFileAttributesA( destpath );
    if (members > 1 && (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)))
    {
        myprintf( "%s: destination directory required for a cabinet with multiple files\n", prog );
        return 0;
    }

    ctx.pattern = filespec;
    ctx.destdir = destpath;
    ctx.matches = 0;
    ctx.fallback = NULL;
    if (members == 1)
    {
        GetFullPathNameA( infile, sizeof(cabpath), cabpath, &cabname );
        ctx.fallback = cabname;
    }

    if (!SetupIterateCabinetA( infile, 0, match_count_callback, &ctx ))
    {
        myprintf( "%s: cabinet extraction failed\n", prog );
        return 1;
    }
    if (ctx.matches == 0)
    {
        myprintf( "%s: no files matching '%s'\n", prog, filespec );
        return 0;
    }

    if (!SetupIterateCabinetA( infile, 0, fextract_callback, &ctx ))
    {
        myprintf( "%s: cabinet extraction failed\n", prog );
        return 1;
    }
    if (ctx.matches > 1)
        myprintf( "%d total files.\n", ctx.matches );
    return 0;
}

int __cdecl main(int argc, char *argv[])
{
    int ret = 0, i, npos = 0;
    const char *filespec = NULL, *pos[2];
    BOOL rename_mode = FALSE;
    char infile[MAX_PATH], outfile[MAX_PATH], actual_name[MAX_PATH];
    char outfile_basename[MAX_PATH], *basename_index;
    UINT comp;
    DWORD req_size, src_size, dst_size;

    for (i = 1; i < argc; i++)
    {
        const char *spec = filespec_from_arg( argv[i] );
        if (spec)
        {
            filespec = spec;
            continue;
        }
        if (option_equal( argv[i], "R" ) || option_equal( argv[i], "r" ))
        {
            rename_mode = TRUE;
            continue;
        }
        if (npos < 2)
            pos[npos++] = argv[i];
    }

    if ((rename_mode && npos != 1) || (!rename_mode && npos != 2))
    {
        myprintf( "Usage:\n" );
        myprintf( "\t%s infile outfile\n", argv[0] );
        myprintf( "\t%s /r infile\n", argv[0] );
        myprintf( "\t%s infile.cab -F:files dest_dir\n", argv[0] );
        return 1;
    }

    GetFullPathNameA( pos[0], sizeof(infile), infile, NULL );

    if (!SetupGetFileCompressionInfoExA( infile, actual_name, sizeof(actual_name), &req_size, &src_size, &dst_size, &comp ))
    {
        myprintf( "%s: can't open input file %s\n", argv[0], infile );
        return 1;
    }

    if (filespec && comp == FILE_COMPRESSION_MSZIP)
        return expand_cab_filespec( argv[0], infile, filespec, pos[1] );

    if (rename_mode)
    {
        switch (comp)
        {
        case FILE_COMPRESSION_MSZIP:
            outfile_basename[0] = 0;
            if (!SetupIterateCabinetA( infile, 0, set_outfile, outfile_basename ))
            {
                myprintf( "%s: can't determine original name\n", argv[0] );
                return 1;
            }
            GetFullPathNameA( infile, sizeof(outfile), outfile, &basename_index );
            *basename_index = 0;
            strcat( outfile, outfile_basename );
            break;
        case FILE_COMPRESSION_WINLZA:
            GetExpandedNameA( infile, outfile_basename );
            break;
        default:
            myprintf( "%s: can't determine original\n", argv[0] );
            return 1;
        }
    }
    else
        GetFullPathNameA( pos[1], sizeof(outfile), outfile, NULL );

    if (!lstrcmpiA( infile, outfile ))
    {
        myprintf( "%s: can't expand file to itself\n", argv[0] );
        return 1;
    }

    if (!output_dir_exists( outfile ))
    {
        myprintf( "%s: can't open output file %s\n", argv[0], outfile );
        return 1;
    }

    switch (comp)
    {
    case FILE_COMPRESSION_MSZIP:
    {
        int members = 0;

        if (!rename_mode)
        {
            if (!SetupIterateCabinetA( infile, 0, count_callback, &members ))
            {
                myprintf( "%s: cabinet extraction failed\n", argv[0] );
                return 1;
            }
            if (members > 1)
            {
                myprintf( "%s: %s contains multiple files; use -F:filespec to choose files (-F:* for all)\n",
                          argv[0], infile );
                return 0;
            }
        }
        if (!SetupIterateCabinetA( infile, 0, extract_callback, outfile ))
        {
            myprintf( "%s: cabinet extraction failed\n", argv[0] );
            return 1;
        }
        break;
    }
    case FILE_COMPRESSION_WINLZA:
    {
        INT hin, hout;
        OFSTRUCT ofin, ofout;
        LONG error;

        if ((hin = LZOpenFileA( infile, &ofin, OF_READ )) < 0)
        {
            myprintf( "%s: can't open input file %s\n", argv[0], infile );
            return 1;
        }
        if ((hout = LZOpenFileA( outfile, &ofout, OF_CREATE | OF_WRITE )) < 0)
        {
            LZClose( hin );
            myprintf( "%s: can't open output file %s\n", argv[0], outfile );
            return 1;
        }
        error = LZCopy( hin, hout );

        LZClose( hin );
        LZClose( hout );

        if (error < 0)
        {
            myprintf( "%s: LZCopy failed, error is %d\n", argv[0], error );
            return 1;
        }
        break;
    }
    default:
        if (!CopyFileA( infile, outfile, FALSE ))
        {
            myprintf( "%s: CopyFileA failed\n", argv[0] );
            return 1;
        }
        break;
    }
    return ret;
}
