//--------------------------------------------------------------------------------------------------
// System Programming                         I/O Lab                                     Fall 2023
//
/// @file
/// @brief resursively traverse directory tree and list all entries
/// @author <yourname>
/// @studid <studentid>
//--------------------------------------------------------------------------------------------------

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <assert.h>
#include <grp.h>
#include <pwd.h>

#define MAX_DIR 64            ///< maximum number of supported directories

/// @brief output control flags
#define F_TREE      0x1       ///< enable tree view
#define F_SUMMARY   0x2       ///< enable summary
#define F_VERBOSE   0x4       ///< turn on verbose mode

/// @brief struct holding the summary
struct summary {
  unsigned int dirs;          ///< number of directories encountered
  unsigned int files;         ///< number of files
  unsigned int links;         ///< number of links
  unsigned int fifos;         ///< number of pipes
  unsigned int socks;         ///< number of sockets

  unsigned long long size;    ///< total size (in bytes)
  unsigned long long blocks;  ///< total number of blocks (512 byte blocks)
};


/// @brief abort the program with EXIT_FAILURE and an optional error message
///
/// @param msg optional error message or NULL
void panic(const char *msg)
{
  if (msg) fprintf(stderr, "%s\n", msg);
  exit(EXIT_FAILURE);
}


/// @brief read next directory entry from open directory 'dir'. Ignores '.' and '..' entries
///
/// @param dir open DIR* stream
/// @retval entry on success
/// @retval NULL on error or if there are no more entries
struct dirent *getNext(DIR *dir)
{
  struct dirent *next;
  int ignore;

  do {
    errno = 0;
    next = readdir(dir);
    if (errno != 0) perror(NULL);
    ignore = next && ((strcmp(next->d_name, ".") == 0) || (strcmp(next->d_name, "..") == 0));
  } while (next && ignore);

  return next;
}


/// @brief qsort comparator to sort directory entries. Sorted by name, directories first.
///
/// @param a pointer to first entry
/// @param b pointer to second entry
/// @retval -1 if a<b
/// @retval 0  if a==b
/// @retval 1  if a>b
static int dirent_compare(const void *a, const void *b)
{
  struct dirent *e1 = (struct dirent*)a;
  struct dirent *e2 = (struct dirent*)b;

  // if one of the entries is a directory, it comes first
  if (e1->d_type != e2->d_type) {
    if (e1->d_type == DT_DIR) return -1;
    if (e2->d_type == DT_DIR) return 1;
  }

  // otherwise sorty by name
  return strcmp(e1->d_name, e2->d_name);
}


/// @brief recursively process directory @a dn and print its tree
///
/// @param dn absolute or relative path string
/// @param pstr prefix string printed in front of each entry
/// @param stats pointer to statistics
/// @param flags output control flags (F_*)
void processDir(const char *dn, const char *pstr, struct summary *stats, unsigned int flags)
{
  // Open the directory
  DIR *dir = opendir(dn);
  if (dir == NULL) { // If directory not opened, print error
    printf("%s%sERROR: %s\n", pstr, flags & F_TREE ? "`-" : "", strerror(errno));
    return;
  }

  // Init an array to store directory entries
  struct dirent *entry, *entries;
  int num_entries = 0;
  entries = NULL;

  // Traverse directory, and malloc entries dynamically 
  while ((entry = getNext(dir)) != NULL) {
    num_entries++;
    // Realloc entries fit in # of files
    struct dirent *new_entries = realloc(entries, sizeof(struct dirent) * num_entries);
    if (new_entries == NULL) { // If not malloc, print error
      printf("%s%sERROR: %s\n", pstr, flags & F_TREE ? "`-" : "", strerror(errno));
      // Free entries and close directory
      free(entries);
      closedir(dir);
      return;
    }
    entries = new_entries; 
    entries[num_entries - 1] = *entry; // allocate new entry
  }

  // Close current directory
  closedir(dir);
  
  // If there's no entries, return
  if(num_entries == 0) {
    return;
  }
  
  // Sort the directories' entry
  qsort(entries, num_entries, sizeof(struct dirent), dirent_compare);
  
  // Traverse and process sorted entries
  for (int i = 0; i < num_entries; i++) {
    // Init dirent and string for file and file data
    struct dirent *e = &entries[i];
    char *nextdir, *nextpstr, *out, *tmp;
    // Build pstr
    if (flags & F_TREE) { // when F_TREE, using |- or `
      if (asprintf(&out, "%s%s", pstr, i<num_entries-1 ? "|-" : "`-") == -1) {
        panic("Out of memory.");
      }
    } else { // when other modes, using " "
      if (asprintf(&out, "%s", pstr) == -1 ) {
        panic("Out of memory.");
      }
    }
    // Store format and directory name
    if (asprintf(&tmp, "%s%s", out, e->d_name) == -1) {
      panic("Out of memory.");
    }
    // free out
    free(out);
    // output strings
    char *user, *group, *size, *blocks, *type;
    // Check file types and store type and number
    if (flags & F_SUMMARY || flags & F_VERBOSE) {
      if (e->d_type == DT_DIR) { // when directory
        stats->dirs++; // count directory
        if (asprintf(&type, "d") == -1) {
          panic("Out of memory.");
        }
      } else if (e->d_type == DT_LNK) { // when link
        stats->links++; // count link
        if (asprintf(&type, "l") == -1) {
          panic("Out of memory.");
        }
      } else if (e->d_type == DT_FIFO) { // when fifo
        stats->fifos++; // count fifo
        if (asprintf(&type, "f") == -1) {
          panic("Out of memory.");
        }
      } else if (e->d_type == DT_SOCK) { // when socket
        stats->socks++; // count socket
        if (asprintf(&type, "s") == -1) {
          panic("Out of memory.");
        }
      } else if (e->d_type == DT_REG) { // when regular file
        stats->files++; // count regular file
        if (asprintf(&type, " ") == -1) {
          panic("Out of memory.");
        }
      } else if (e->d_type == DT_BLK) { // when bolck
        if (asprintf(&type, "b") == -1) {
          panic("Out of memory.");
        }
      } else if (e->d_type == DT_CHR) { //when char
        if (asprintf(&type, "c") == -1) {
          panic("Out of memory.");
        }
      }
    }
    // When F_verbose or F_summary
    if (flags & F_VERBOSE || flags & F_SUMMARY) {
      char *file_name;
      // Build file name
      if (asprintf(&file_name, "%s%s%s", dn, "/", e->d_name) == -1) { 
        panic("Out of memory.");
      }
      // Stat for file data
      struct stat st;

      // Read metadata of file
      if (lstat(file_name, &st) == 0) {
        if (flags & F_VERBOSE) { // When F_verbose mode, store metadata of file
          struct passwd *pw;
          struct group *grp;
          // Get pwuid and grgid
          pw = getpwuid(st.st_uid);
          grp = getgrgid(st.st_gid);

          if (asprintf(&user, "%s", pw->pw_name) == -1) { // store user name
            panic("Out of memory.");
          }
          if (asprintf(&group, "%s", grp->gr_name) == -1) { // store group name
            panic("Out of memory.");
          }

          if (asprintf(&size, "%ld", st.st_size) == -1) { // store size
            panic("Out of memory.");
          }
          if (asprintf(&blocks, "%ld", st.st_blocks) == -1) { // store # of blocks
            panic("Out of memory.");
          }
        }
        // Add size and blocks to stats
        stats->size += st.st_size;
        stats->blocks += st.st_blocks;
      } else { // when there's no metadata
        if (flags & F_VERBOSE) {
          char formatted_tmp[55]; // 54 characters + null-terminator
          int tmp_len = strlen(tmp);

          if (tmp_len > 54) { // when tmp_len is over 54 characters
              strncpy(formatted_tmp, tmp, 51); // Take first 51 characters
              strcpy(formatted_tmp + 51, "..."); // Append "..."
          } else { // / when tmp_len is equal or less than 54 characters
              strncpy(formatted_tmp, tmp, tmp_len);
              formatted_tmp[tmp_len] = '\0'; // Null-terminate the string
          }
          printf("%-54s  %s\n", formatted_tmp, strerror(errno)); // print error message
          continue;
        }
      }
      // Free file_name string
      free(file_name);
    }

    // Print Logic
    if (flags & F_VERBOSE) {
      char formatted_tmp[55]; // 54 characters + null-terminator
      int tmp_len = strlen(tmp);

      if (tmp_len > 54) { // when tmp_len is over 54 characters
          strncpy(formatted_tmp, tmp, 51); // Take first 51 characters
          strcpy(formatted_tmp + 51, "..."); // Append "..."
      } else { // / when tmp_len is equal or less than 54 characters
          strncpy(formatted_tmp, tmp, tmp_len);
          formatted_tmp[tmp_len] = '\0'; // add null-terminate the string
      }
      printf("%-54s  %8s:%-8s  %10s  %8s  %s\n", formatted_tmp, user, group, size, blocks, type); // print detail line
      // Free string memory
      free(user);
      free(group);
      free(size);
      free(blocks);
      free(type);
    } else {
      printf("%s\n", tmp); // print file name
    }
    // When current file is directory
    if (e->d_type == DT_DIR) {
      if (asprintf(&nextpstr, flags & F_TREE && i<num_entries-1 ? "%s| " : "%s  ", pstr) == -1) { // build indentation
        panic("Out of memory.");
      }

      if (asprintf(&nextdir, "%s%s%s", dn, "/", e->d_name) == -1) { // build full file path
        panic("Out of memory.");
      }
      // processDir at nextdir
      processDir(nextdir, nextpstr, stats, flags); 
      // Free string memory
      free(nextdir);
      free(nextpstr);
    }
  }
  free(entries); // free entries
}


/// @brief print program syntax and an optional error message. Aborts the program with EXIT_FAILURE
///
/// @param argv0 command line argument 0 (executable)
/// @param error optional error (format) string (printf format) or NULL
/// @param ... parameter to the error format string
void syntax(const char *argv0, const char *error, ...)
{
  if (error) {
    va_list ap;

    va_start(ap, error);
    vfprintf(stderr, error, ap);
    va_end(ap);

    printf("\n\n");
  }

  assert(argv0 != NULL);

  fprintf(stderr, "Usage %s [-t] [-s] [-v] [-h] [path...]\n"
                  "Gather information about directory trees. If no path is given, the current directory\n"
                  "is analyzed.\n"
                  "\n"
                  "Options:\n"
                  " -t        print the directory tree (default if no other option specified)\n"
                  " -s        print summary of directories (total number of files, total file size, etc)\n"
                  " -v        print detailed information for each file. Turns on tree view.\n"
                  " -h        print this help\n"
                  " path...   list of space-separated paths (max %d). Default is the current directory.\n",
                  basename(argv0), MAX_DIR);

  exit(EXIT_FAILURE);
}


/// @brief program entry point
int main(int argc, char *argv[])
{
  //
  // default directory is the current directory (".")
  //
  const char CURDIR[] = ".";
  const char *directories[MAX_DIR];
  int   ndir = 0;

  struct summary tstat;
  unsigned int flags = 0;

  //
  // parse arguments
  //
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      // format: "-<flag>"
      if      (!strcmp(argv[i], "-t")) flags |= F_TREE;
      else if (!strcmp(argv[i], "-s")) flags |= F_SUMMARY;
      else if (!strcmp(argv[i], "-v")) flags |= F_VERBOSE;
      else if (!strcmp(argv[i], "-h")) syntax(argv[0], NULL);
      else syntax(argv[0], "Unrecognized option '%s'.", argv[i]);
    } else {
      // anything else is recognized as a directory
      if (ndir < MAX_DIR) {
        directories[ndir++] = argv[i];
      } else {
        printf("Warning: maximum number of directories exceeded, ignoring '%s'.\n", argv[i]);
      }
    }
  }

  // If no directory was specified, use the current directory
  if (ndir == 0) directories[ndir++] = CURDIR;

  // Process each directory
  struct summary dstat;
  // Init the struct for total stat
  memset(&tstat, 0, sizeof(tstat));
  for (int i = 0; i < ndir; i++) { // processdir in each directories
    memset(&dstat, 0, sizeof(dstat)); // init the struct for directory stat
    if (flags & F_SUMMARY) { // when summary mode
      if (flags & F_VERBOSE) { // when detail mode, print all categories
        printf("%-54s  %8s:%-8s  %10s  %8s %s \n", "Name", "User", "Group", "Size", "Blocks", "Type");
      } else { // when not detail mode, print only `name`
        printf("%s\n", "Name");
      }
      printf("----------------------------------------------------------------------------------------------------\n");
    }
    printf("%s\n", directories[i]); // print directory name
    // Travelse each directories
    if (flags & F_TREE) { // when tree mdoe, 
      processDir(directories[i], "", &dstat, flags);
    } else { // when not tree mode, 
      processDir(directories[i], "  ", &dstat, flags);
    }
    // When summary mode, print summary statement
    if (flags & F_SUMMARY) {
      printf("----------------------------------------------------------------------------------------------------\n");
      // Init strings for storing notation strings and summary statement
      char *file_notation, *dir_notation, *link_notation, *pipe_notation, *socket_notation;
      char *summary_statement;
      // Decide notation depending on whether it's singular or plural
      if(asprintf(&file_notation, "%s", dstat.files == 1 ? "file" : "files") == -1) { // for file notation
        panic("Out of memory.");
      }
      if(asprintf(&dir_notation, "%s", dstat.dirs == 1 ? "directory" : "directories") == -1) { // for dir notation
        panic("Out of memory.");
      }
      if(asprintf(&link_notation, "%s", dstat.links == 1 ? "link" : "links") == -1) { // for link notation
        panic("Out of memory.");
      }
      if(asprintf(&pipe_notation, "%s", dstat.fifos == 1 ? "pipe" : "pipes") == -1) { // for pipe notation
        panic("Out of memory.");
      }
      if(asprintf(&socket_notation, "%s", dstat.socks == 1 ? "socket" : "sockets") == -1) { // for socket notation
        panic("Out of memory.");
      }
      // Build summary statement
      if(asprintf(&summary_statement, "%d %s, %d %s, %d %s, %d %s, and %d %s", dstat.files, file_notation, dstat.dirs, dir_notation, dstat.links, link_notation, dstat.fifos, pipe_notation, dstat.socks, socket_notation) == -1) {
        panic("Out of memory.");
      }

    if (flags & F_VERBOSE) { // when detail mode, print summary statement limit 86 char, size, and # of blocks
      if (strlen(summary_statement) > 68) { // when summary statement over 68 char
        char truncated_summary[69];
        strncpy(truncated_summary, summary_statement, 68); // truncate summary statement
        truncated_summary[68] = '\0'; // add null-terminate the string
        printf("%-68s   %14llu %9llu\n", truncated_summary, dstat.size, dstat.blocks);
      } else { // when summary statement less or equal than 68 characters
        printf("%-68s   %14llu %9llu\n", summary_statement, dstat.size, dstat.blocks);
      }
    } else { // when other mode, print only summary statement
      printf("%s\n", summary_statement);
    }
      // print new line
      printf("\n");
    }
    // Add directory stats to total stats
    tstat.dirs += dstat.dirs;
    tstat.files += dstat.files;
    tstat.links += dstat.links;
    tstat.fifos += dstat.fifos;
    tstat.socks += dstat.socks;
    tstat.size += dstat.size;
    tstat.blocks += dstat.blocks;
  }
  // When summary mode and ndir is over 1, print analyzed result(# of files, directories ...)
  if ((flags & F_SUMMARY) && (ndir > 1)) {
    printf("Analyzed %d directories:\n", ndir);
    printf("  total # of files:        %16d\n", tstat.files);
    printf("  total # of directories:  %16d\n", tstat.dirs);
    printf("  total # of links:        %16d\n", tstat.links);
    printf("  total # of pipes:        %16d\n", tstat.fifos);
    printf("  total # of sockets:      %16d\n", tstat.socks);
    // When detail mode, print total file size and # of blocks
    if (flags & F_VERBOSE) {
      printf("  total file size:         %16llu\n", tstat.size);
      printf("  total # of blocks:       %16llu\n", tstat.blocks);
    }
  }
  
  return EXIT_SUCCESS;
}
