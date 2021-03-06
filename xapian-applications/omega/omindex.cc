/* omindex.cc: index static documents into the omega db
 *
 * Copyright 1999,2000,2001 BrightStation PLC
 * Copyright 2001,2005 James Aylett
 * Copyright 2001,2002 Ananova Ltd
 * Copyright 2002,2003,2004,2005,2006,2007,2008,2009,2010 Olly Betts
 * Copyright 2009 Frank J Bruzzaniti
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <config.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <map>
#include <vector>

#include <sys/types.h>
#include "safeunistd.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "safefcntl.h"
#include "safeerrno.h"
#include <ctime>

#include <xapian.h>

#include "commonhelp.h"
#include "diritor.h"
#include "hashterm.h"
#include "loadfile.h"
#include "md5wrap.h"
#include "metaxmlparse.h"
#include "myhtmlparse.h"
#include "runfilter.h"
#include "sample.h"
#include "str.h"
#include "stringutils.h"
#include "utf8convert.h"
#include "utils.h"
#include "values.h"
#include "xmlparse.h"
#include "xpsxmlparse.h"

#include "gnu_getopt.h"

#ifndef HAVE_MKDTEMP
extern char * mkdtemp(char *);
#endif

using namespace std;

#define TITLE_SIZE 128
#define SAMPLE_SIZE 512

#define PROG_NAME "omindex"
#define PROG_DESC "Index static website data via the filesystem"

static bool skip_duplicates = false;
static bool follow_symlinks = false;
static bool spelling = false;

static string dbpath;
static string root;
static string indexroot;
static string baseurl;
static Xapian::WritableDatabase db;
static Xapian::Stem stemmer("english");
static Xapian::TermGenerator indexer;
static vector<bool> updated;
static string tmpdir;

static time_t last_mod_max;

inline static bool
p_notalnum(unsigned int c)
{
    return !isalnum(static_cast<unsigned char>(c));
}

static string
shell_protect(const string & file)
{
    string safefile = file;
#ifdef __WIN32__
    bool need_to_quote = false;
    for (string::iterator i = safefile.begin(); i != safefile.end(); ++i) {
	unsigned char ch = *i;
	if (!isalnum(ch) && ch < 128) {
	    if (ch == '/') {
		// Convert Unix path separators to backslashes.  C library
		// functions understand "/" in paths, but external commands
		// generally don't, and also may interpret a leading '/' as
		// introducing a command line option.
		*i = '\\';
	    } else if (ch == ' ') {
		need_to_quote = true;
	    } else if (ch < 32 || strchr("<>\"|*?", ch)) {
		// Check for invalid characters in the filename.
		string m("Invalid character '");
		m += ch;
		m += "' in filename \"";
		m += file;
		m += '"';
		throw m;
	    }
	}
    }
    if (safefile[0] == '-') {
	// If the filename starts with a '-', protect it from being treated as
	// an option by prepending ".\".
	safefile.insert(0, ".\\");
    }
    if (need_to_quote) {
	safefile.insert(0, "\"");
	safefile += '"';
    }
#else
    string::size_type p = 0;
    if (!safefile.empty() && safefile[0] == '-') {
	// If the filename starts with a '-', protect it from being treated as
	// an option by prepending "./".
	safefile.insert(0, "./");
	p = 2;
    }
    while (p < safefile.size()) {
	// Don't escape some safe characters which are common in filenames.
	unsigned char ch = safefile[p];
	if (!isalnum(ch) && strchr("/._-", ch) == NULL) {
	    safefile.insert(p, "\\");
	    ++p;
	}
	++p;
    }
#endif
    return safefile;
}

static bool ensure_tmpdir() {
    if (!tmpdir.empty()) return true;

    const char * p = getenv("TMPDIR");
    if (!p) p = "/tmp";
    char * dir_template = new char[strlen(p) + 15 + 1];
    strcpy(dir_template, p);
    strcat(dir_template, "/omindex-XXXXXX");
    p = mkdtemp(dir_template);
    if (p) {
	tmpdir.assign(dir_template);
	tmpdir += '/';
    }
    delete [] dir_template;
    return (p != NULL);
}

static string
file_to_string(const string &file)
{
    string out;
    if (!load_file(file, out)) throw ReadError();
    return out;
}

static void
get_pdf_metainfo(const string & safefile, string &title, string &keywords)
{
    try {
	string pdfinfo = stdout_to_string("pdfinfo -enc UTF-8 " + safefile);

	string::size_type idx;

	if (strncmp(pdfinfo.c_str(), "Title:", 6) == 0) {
	    idx = 0;
	} else {
	    idx = pdfinfo.find("\nTitle:");
	}
	if (idx != string::npos) {
	    if (idx) ++idx;
	    idx = pdfinfo.find_first_not_of(' ', idx + 6);
	    string::size_type end = pdfinfo.find('\n', idx);
	    if (end != string::npos) {
		if (pdfinfo[end - 1] == '\r') --end;
		end -= idx;
	    }
	    title.assign(pdfinfo, idx, end);
	}

	if (strncmp(pdfinfo.c_str(), "Keywords:", 9) == 0) {
	    idx = 0;
	} else {
	    idx = pdfinfo.find("\nKeywords:");
	}
	if (idx != string::npos) {
	    if (idx) ++idx;
	    idx = pdfinfo.find_first_not_of(' ', idx + 9);
	    string::size_type end = pdfinfo.find('\n', idx);
	    if (end != string::npos) {
		if (pdfinfo[end - 1] == '\r') --end;
		end -= idx;
	    }
	    keywords.assign(pdfinfo, idx, end);
	}
    } catch (ReadError) {
	// It's probably best to index the document even if pdfinfo fails.
    }
}

static void
index_file(const string &url, const string &mimetype, time_t last_mod, off_t size)
{
    string file = root + url;
    string title, sample, keywords, dump;

    cout << "Indexing \"" << url << "\" as " << mimetype << " ... " << flush;

    string urlterm("U");
    urlterm += baseurl;
    urlterm += url;

    if (urlterm.length() > MAX_SAFE_TERM_LENGTH)
	urlterm = hash_long_term(urlterm, MAX_SAFE_TERM_LENGTH);

    Xapian::docid did = 0; 
    if (skip_duplicates) {
        if (db.term_exists(urlterm)) {
	    cout << "duplicate. Ignored." << endl;
	    return;
	}
    } else {
	// If last_mod > last_mod_max, we know for sure that the file is new
	// or updated.
	if (last_mod <= last_mod_max) {
	    Xapian::PostingIterator p = db.postlist_begin(urlterm);
	    if (p != db.postlist_end(urlterm)) {
		did = *p;
		Xapian::Document doc = db.get_document(did);
		string value = doc.get_value(VALUE_LASTMOD);
		time_t old_last_mod = binary_string_to_int(value);
		if (last_mod <= old_last_mod) {
		    cout << "Already indexed." << endl;
		    // The docid should be in updated - the only valid
		    // exception is if the URL was long and hashed to the
		    // same URL as an existing document indexed in the same
		    // batch.
		    if (usual(did < updated.size())) updated[did] = true;
		    return;
		}
	    }
	}
    }

    string md5;
    if (mimetype == "text/html") {
	string text;
	try {
	    text = file_to_string(file);
	} catch (ReadError) {
	    cout << "can't read \"" << file << "\" - skipping" << endl;
	    return;
	}
	MyHtmlParser p;
	try {
	    // Default HTML character set is latin 1, though not specifying one
	    // is deprecated these days.
	    p.parse_html(text, "iso-8859-1", false);
	} catch (const string & newcharset) {
	    try {
		p.reset();
		p.parse_html(text, newcharset, true);
	    } catch (bool) {
	    }
	} catch (bool) {
	    // MyHtmlParser throws a bool to abandon parsing at </body> or when
	    // indexing is disallowed
	}
	if (!p.indexing_allowed) {
	    cout << "indexing disallowed by meta tag - skipping" << endl;
	    return;
	}
	dump = p.dump;
	title = p.title;
	keywords = p.keywords;
	sample = p.sample;
	md5_string(text, md5);
    } else if (mimetype == "text/plain") {
	try {
	    // Currently we assume that text files are UTF-8 unless they have a
	    // byte-order mark.
	    dump = file_to_string(file);
	    md5_string(dump, md5);

	    // Look for Byte-Order Mark (BOM).
	    if (startswith(dump, "\xfe\xff") || startswith(dump, "\xff\xfe")) {
		// UTF-16 in big-endian/little-endian order - we just convert
		// it as "UTF-16" and let the conversion handle the BOM as that
		// way we avoid the copying overhead of erasing 2 bytes from
		// the start of dump.
		convert_to_utf8(dump, "UTF-16");
	    } else if (startswith(dump, "\xef\xbb\xbf")) {
		// UTF-8 with stupid Windows not-the-byte-order mark.
		dump.erase(0, 3);
	    } else {
		// FIXME: What charset is the file?  Look at contents?
	    }
	} catch (ReadError) {
	    cout << "can't read \"" << file << "\" - skipping" << endl;
	    return;
	}
    } else if (mimetype == "application/pdf") {
	string safefile = shell_protect(file);
	string cmd = "pdftotext -enc UTF-8 " + safefile + " -";
	try {
	    dump = stdout_to_string(cmd);
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
	get_pdf_metainfo(safefile, title, keywords);
    } else if (mimetype == "application/postscript") {
	// There simply doesn't seem to be a Unicode capable PostScript to
	// text converter (e.g. pstotext always outputs ISO-8859-1).  The only
	// solution seems to be to convert via PDF using ps2pdf and then
	// pdftotext.  This gives plausible looking UTF-8 output for some
	// Chinese PostScript files I found using Google.  It also has the
	// benefit of allowing us to extract meta information from PostScript
	// files.
	if (!ensure_tmpdir()) {
	    // FIXME: should this be fatal?  Or disable indexing postscript?
	    cout << "Couldn't create temporary directory (" << strerror(errno) << ") - skipping" << endl;
	    return;
	}
	string tmpfile = tmpdir + "/tmp.pdf";
	string safetmp = shell_protect(tmpfile);
	string cmd = "ps2pdf " + shell_protect(file) + " " + safetmp;
	try {
	    (void)stdout_to_string(cmd);
	    cmd = "pdftotext -enc UTF-8 " + safetmp + " -";
	    dump = stdout_to_string(cmd);
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    unlink(tmpfile.c_str());
	    return;
	} catch (...) {
	    unlink(tmpfile.c_str());
	    throw;
	}
	try {
	    get_pdf_metainfo(safetmp, title, keywords);
	} catch (...) {
	    unlink(tmpfile.c_str());
	    throw;
	}
	unlink(tmpfile.c_str());
    } else if (startswith(mimetype, "application/vnd.sun.xml.") ||
	       startswith(mimetype, "application/vnd.oasis.opendocument."))
    {
	// Inspired by http://mjr.towers.org.uk/comp/sxw2text
	string safefile = shell_protect(file);
	string cmd = "unzip -p " + safefile + " content.xml";
	try {
	    XmlParser xmlparser;
	    xmlparser.parse_html(stdout_to_string(cmd));
	    dump = xmlparser.dump;
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}

	cmd = "unzip -p " + safefile + " meta.xml";
	try {
	    MetaXmlParser metaxmlparser;
	    metaxmlparser.parse_html(stdout_to_string(cmd));
	    title = metaxmlparser.title;
	    keywords = metaxmlparser.keywords;
	    sample = metaxmlparser.sample;
	} catch (ReadError) {
	    // It's probably best to index the document even if this fails.
	}
    } else if (mimetype == "application/msword") {
	string cmd = "antiword -mUTF-8.txt " + shell_protect(file);
	try {
	    dump = stdout_to_string(cmd);
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else if (mimetype == "application/vnd.ms-excel") {
	string cmd = "xls2csv -q0 -dutf-8 " + shell_protect(file);
	try {
	    dump = stdout_to_string(cmd);
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else if (mimetype == "application/vnd.ms-powerpoint") {
	string cmd = "catppt -dutf-8 " + shell_protect(file);
	try {
	    dump = stdout_to_string(cmd);
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else if (startswith(mimetype, "application/vnd.openxmlformats-officedocument.")) {
	const char * args = NULL;
	string tail(mimetype, 46);
	if (startswith(tail, "wordprocessingml.")) {
	    args = " word/document.xml";
	} else if (startswith(tail, "spreadsheetml.")) {
	    args = " xl/sharedStrings.xml";
	} else if (startswith(tail, "presentationml.")) {
	    // unzip returns exit code 11 if a file to extract wasn't found
	    // which we want to ignore, because there may be no notesSlides
	    // or comments.
	    args = " ppt/slides/slide*.xml ppt/notesSlides/notesSlide*.xml ppt/comments/comment*.xml 2>/dev/null||test $? = 11";
	} else {
	    // Don't know how to index this type.
	    cout << "unknown Office 2007 MIME subtype - skipping" << endl;
	    return;
	}
	string safefile = shell_protect(file);
	string cmd = "unzip -p " + safefile + args;
	try {
	    XmlParser xmlparser;
	    xmlparser.parse_html(stdout_to_string(cmd));
	    dump = xmlparser.dump;
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else if (mimetype == "application/vnd.wordperfect") {
	// Looking at the source of wpd2html and wpd2text I think both output
	// utf-8, but it's hard to be sure without sample Unicode .wpd files
	// as they don't seem to be at all well documented.
	string cmd = "wpd2text " + shell_protect(file);
	try {
	    dump = stdout_to_string(cmd);
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else if (mimetype == "application/vnd.ms-works") {
	// wps2text produces UTF-8 output from the sample files I've tested.
	string cmd = "wps2text " + shell_protect(file);
	try {
	    dump = stdout_to_string(cmd);
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else if (mimetype == "application/x-abiword") {
	// FIXME: Implement support for metadata.
	try {
	    XmlParser xmlparser;
	    string text = file_to_string(file);
	    xmlparser.parse_html(text);
	    dump = xmlparser.dump;
	    md5_string(text, md5);
	} catch (ReadError) {
	    cout << "can't read \"" << file << "\" - skipping" << endl;
	    return;
	}
    } else if (mimetype == "application/x-abiword-compressed") {
	// FIXME: Implement support for metadata.
	string cmd = "gzip -dc " + shell_protect(file);
	try {
	    XmlParser xmlparser;
	    xmlparser.parse_html(stdout_to_string(cmd));
	    dump = xmlparser.dump;
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else if (mimetype == "text/rtf") {
	// The --text option unhelpfully converts all non-ASCII characters to
	// "?" so we use --html instead, which produces HTML entities.
	string cmd = "unrtf --nopict --html 2>/dev/null " + shell_protect(file);
	MyHtmlParser p;
	try {
	    // No point going looking for charset overrides as unrtf doesn't
	    // produce them.
	    p.parse_html(stdout_to_string(cmd), "iso-8859-1", true);
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	} catch (bool) {
	    // MyHtmlParser throws a bool to abandon parsing at </body> or when
	    // indexing is disallowed
	}
	if (!p.indexing_allowed) {
	    cout << "indexing disallowed by meta tag - skipping" << endl;
	    return;
	}
	dump = p.dump;
	title = p.title;
	keywords = p.keywords;
	sample = p.sample;
    } else if (mimetype == "text/x-perl") {
	// pod2text's output character set doesn't seem to be documented, but
	// from inspecting the source it looks like it's probably iso-8859-1.
	string cmd = "pod2text " + shell_protect(file);
	try {
	    dump = stdout_to_string(cmd);
	    convert_to_utf8(dump, "ISO-8859-1");
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else if (mimetype == "application/x-dvi") {
	// FIXME: -e0 means "UTF-8", but that results in "fi", "ff", "ffi", etc
	// appearing as single ligatures.  For European languages, it's
	// actually better to use -e2 (ISO-8859-1) and then convert, so let's
	// do that for now until we handle Unicode "compatibility
	// decompositions".
	string cmd = "catdvi -e2 -s " + shell_protect(file);
	try {
	    dump = stdout_to_string(cmd);
	    convert_to_utf8(dump, "ISO-8859-1");
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else if (mimetype == "image/vnd.djvu") {
	// Output is UTF-8 according to "man djvutxt".  Generally this seems to
	// be true, though some examples from djvu.org generate isolated byte
	// 0x95 in a context which suggests it might be intended to be a bullet
	// (as it is in CP1250).
	string cmd = "djvutxt " + shell_protect(file);
	try {
	    dump = stdout_to_string(cmd);
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else if (mimetype == "application/vnd.ms-xpsdocument") {
	string safefile = shell_protect(file);
	string cmd = "unzip -p " + safefile + " Documents/1/Pages/*.fpage";
	try {
	    XpsXmlParser xpsparser;
	    dump = stdout_to_string(cmd);
	    // Look for Byte-Order Mark (BOM).
	    if (startswith(dump, "\xfe\xff") || startswith(dump, "\xff\xfe")) {
		// UTF-16 in big-endian/little-endian order - we just convert
		// it as "UTF-16" and let the conversion handle the BOM as that
		// way we avoid the copying overhead of erasing 2 bytes from
		// the start of dump.
		convert_to_utf8(dump, "UTF-16");
	    }
	    xpsparser.parse_html(dump);
	    dump = xpsparser.dump;
	} catch (ReadError) {
	    cout << "\"" << cmd << "\" failed - skipping" << endl;
	    return;
	}
    } else {
	// Don't know how to index this type.
	cout << "unknown MIME type - skipping" << endl;
	return;
    }

    // Compute the MD5 of the file if we haven't already.
    if (md5.empty() && md5_file(file, md5) == 0) {
	cout << "failed to read file to calculate MD5 checksum - skipping" << endl;
	return;
    }

    // Produce a sample
    if (sample.empty()) {
	sample = generate_sample(dump, SAMPLE_SIZE);
    } else {
	sample = generate_sample(sample, SAMPLE_SIZE);
    }

    // Put the data in the document
    Xapian::Document newdocument;
    string record = "url=" + baseurl + url + "\nsample=" + sample;
    if (!title.empty()) {
	record += "\ncaption=" + generate_sample(title, TITLE_SIZE);
    }
    record += "\ntype=" + mimetype;
    if (last_mod != (time_t)-1) {
	record += "\nmodtime=";
	record += str(last_mod);
    }
    if (size) {
	record += "\nsize=";
	record += str(size);
    }
    newdocument.set_data(record);

    // Index the title, document text, and keywords.
    indexer.set_document(newdocument);
    if (!title.empty()) {
	indexer.index_text(title, 5);
	indexer.increase_termpos(100);
    }
    if (!dump.empty()) {
	indexer.index_text(dump);
    }
    if (!keywords.empty()) {
	indexer.increase_termpos(100);
	indexer.index_text(keywords);
    }

    newdocument.add_term("T" + mimetype); // mimeType
    string::size_type j;
    j = find_if(baseurl.begin(), baseurl.end(), p_notalnum) - baseurl.begin();
    if (j > 0 && baseurl.substr(j, 3) == "://") {
	j += 3;
	string::size_type k = baseurl.find('/', j);
	if (k == string::npos) {
	    newdocument.add_term("P/"); // Path
	    newdocument.add_term("H" + baseurl.substr(j));
	} else {
	    newdocument.add_term("P" + baseurl.substr(k)); // Path
	    string::const_iterator l;
	    l = find(baseurl.begin() + j, baseurl.begin() + k, ':');
	    string::size_type host_len = l - baseurl.begin() - j;
	    newdocument.add_term("H" + baseurl.substr(j, host_len)); // Host
	}
    } else {
	newdocument.add_term("P" + baseurl); // Path
    }

    struct tm *tm = localtime(&last_mod);
    string date_term = "D" + date_to_string(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    newdocument.add_term(date_term); // Date (YYYYMMDD)
    date_term.resize(7);
    date_term[0] = 'M';
    newdocument.add_term(date_term); // Month (YYYYMM)
    date_term.resize(5);
    date_term[0] = 'Y';
    newdocument.add_term(date_term); // Year (YYYY)

    newdocument.add_term(urlterm); // Url

    // Add last_mod as a value to allow "sort by date".
    newdocument.add_value(VALUE_LASTMOD,
			  int_to_binary_string((uint32_t)last_mod));

    // Add MD5 as a value to allow duplicate documents to be collapsed together.
    newdocument.add_value(VALUE_MD5, md5);

    if (!skip_duplicates) {
	// If this document has already been indexed, update the existing
	// entry.
	if (did) {
	    // We already found out the document id above.
	    db.replace_document(did, newdocument);
	} else if (last_mod <= last_mod_max) {
	    // We checked for the UID term and didn't find it.
	    did = db.add_document(newdocument);
	} else {
	    did = db.replace_document(urlterm, newdocument);
	}
	if (did < updated.size()) {
	    updated[did] = true;
	    cout << "updated." << endl;
	} else {
	    cout << "added." << endl;
	}
    } else {
	// If this were a duplicate, we'd have skipped it above.
	db.add_document(newdocument);
	cout << "added." << endl;
    }
}

static void
index_directory(size_t depth_limit, const string &dir,
		map<string, string>& mime_map)
{
    string path = root + indexroot + dir;

    cout << "[Entering directory " << dir << "]" << endl;

    DirectoryIterator d(follow_symlinks);
    try {
	d.start(path);
	while (d.next()) try {
	    string url = dir;
	    if (!url.empty() && url[url.size() - 1] != '/') url += '/';
	    url += d.leafname();
	    string file = root + indexroot + url;
	    switch (d.get_type()) {
		case DirectoryIterator::DIRECTORY:
		    if (depth_limit == 1) continue;
		    try {
			size_t new_limit = depth_limit;
			if (new_limit) --new_limit;
			index_directory(new_limit, url, mime_map);
		    } catch (...) {
			cout << "Caught unknown exception in index_directory, rethrowing" << endl;
			throw;
		    }
		    continue;
		case DirectoryIterator::REGULAR_FILE: {
		    string ext;
		    string::size_type dot = url.find_last_of('.');
		    if (dot != string::npos) ext = url.substr(dot + 1);

		    map<string,string>::iterator mt = mime_map.find(ext);
		    if (mt == mime_map.end()) {
			// If the extension isn't found, see if the lower-cased
			// version (if different) is found.
			bool changed = false;
			string::iterator i;
			for (i = ext.begin(); i != ext.end(); ++i) {
			    if (*i >= 'A' && *i <= 'Z') {
				*i = tolower(*i);
				changed = true;
			    }
			}
			if (changed) mt = mime_map.find(ext);
		    }
		    if (mt != mime_map.end()) {
			if (mt->second.empty()) {
			    cout << "Skipping file, required filter not "
				    "installed: \"" << file << "\""
				 << endl;
			    continue;
			}

			// Only check the file size if we recognise the
			// extension to avoid a call to stat()/lstat() for
			// files we can't handle when readdir() tells us the
			// file type.
			off_t size = d.get_size();
			if (size == 0) {
			    cout << "Skipping empty file: \"" << file << "\""
				 << endl;
			    continue;
			}

			// It's in our MIME map so we know how to index it.
			const string & mimetype = mt->second;
			try {
			    time_t mtime = d.get_mtime();
			    index_file(indexroot + url, mimetype, mtime, size);
			} catch (NoSuchFilter) {
			    // FIXME: we ought to ignore by mime-type not
			    // extension.
			    cout << "Filter for \"" << mimetype
				 << "\" not installed - ignoring extension \""
				 << ext << "\"" << endl;
			    mt->second = string();
			}
		    } else {
			cout << "Unknown extension: \"" << file
			     << "\" - skipping" << endl;
		    }
		    continue;
		}
		default:
		    cout << "Not a regular file \"" << file
			 << "\" - skipping" << endl;
	    }
	} catch (const std::string & error) {
	    cout << error << " - skipping" << endl;
	    continue;
	}
    } catch (const std::string & error) {
	cout << error << " - skipping directory" << endl;
	return;
    }
}

int
main(int argc, char **argv)
{
    // If overwrite is true, the database will be created anew even if it
    // already exists.
    bool overwrite = false;
    // If preserve_unupdated is false, delete any documents we don't
    // replace (if in replace duplicates mode)
    bool preserve_unupdated = false;
    size_t depth_limit = 0;

    static const struct option longopts[] = {
	{ "help",	no_argument,		NULL, 'h' },
	{ "version",	no_argument,		NULL, 'v' },
	{ "overwrite",	no_argument,		NULL, 'o' },
	{ "duplicates",	required_argument,	NULL, 'd' },
	{ "preserve-nonduplicates",	no_argument,	NULL, 'p' },
	{ "db",		required_argument,	NULL, 'D' },
	{ "url",	required_argument,	NULL, 'U' },
	{ "mime-type",	required_argument,	NULL, 'M' },
	{ "depth-limit",required_argument,	NULL, 'l' },
	{ "follow",	no_argument,		NULL, 'f' },
	{ "stemmer",	required_argument,	NULL, 's' },
	{ "spelling",	no_argument,		NULL, 'S' },
	{ 0, 0, NULL, 0 }
    };

    int getopt_ret;

    map<string, string> mime_map;
    // Plain text:
    mime_map["txt"] = "text/plain";
    mime_map["text"] = "text/plain";

    // HTML:
    mime_map["html"] = "text/html";
    mime_map["htm"] = "text/html";
    mime_map["shtml"] = "text/html";
    mime_map["php"] = "text/html"; // Our HTML parser knows to ignore PHP code.

    // PDF:
    mime_map["pdf"] = "application/pdf";

    // PostScript:
    mime_map["ps"] = "application/postscript";
    mime_map["eps"] = "application/postscript";
    mime_map["ai"] = "application/postscript";

    // OpenDocument:
    // FIXME: need to find sample documents to test all of these.
    mime_map["odt"] = "application/vnd.oasis.opendocument.text";
    mime_map["ods"] = "application/vnd.oasis.opendocument.spreadsheet";
    mime_map["odp"] = "application/vnd.oasis.opendocument.presentation";
    mime_map["odg"] = "application/vnd.oasis.opendocument.graphics";
    mime_map["odc"] = "application/vnd.oasis.opendocument.chart";
    mime_map["odf"] = "application/vnd.oasis.opendocument.formula";
    mime_map["odb"] = "application/vnd.oasis.opendocument.database";
    mime_map["odi"] = "application/vnd.oasis.opendocument.image";
    mime_map["odm"] = "application/vnd.oasis.opendocument.text-master";
    mime_map["ott"] = "application/vnd.oasis.opendocument.text-template";
    mime_map["ots"] = "application/vnd.oasis.opendocument.spreadsheet-template";
    mime_map["otp"] = "application/vnd.oasis.opendocument.presentation-template";
    mime_map["otg"] = "application/vnd.oasis.opendocument.graphics-template";
    mime_map["otc"] = "application/vnd.oasis.opendocument.chart-template";
    mime_map["otf"] = "application/vnd.oasis.opendocument.formula-template";
    mime_map["oti"] = "application/vnd.oasis.opendocument.image-template";
    mime_map["oth"] = "application/vnd.oasis.opendocument.text-web";

    // OpenOffice/StarOffice documents:
    mime_map["sxc"] = "application/vnd.sun.xml.calc";
    mime_map["stc"] = "application/vnd.sun.xml.calc.template";
    mime_map["sxd"] = "application/vnd.sun.xml.draw";
    mime_map["std"] = "application/vnd.sun.xml.draw.template";
    mime_map["sxi"] = "application/vnd.sun.xml.impress";
    mime_map["sti"] = "application/vnd.sun.xml.impress.template";
    mime_map["sxm"] = "application/vnd.sun.xml.math";
    mime_map["sxw"] = "application/vnd.sun.xml.writer";
    mime_map["sxg"] = "application/vnd.sun.xml.writer.global";
    mime_map["stw"] = "application/vnd.sun.xml.writer.template";

    // MS Office 2007 formats:
    mime_map["docx"] = "application/vnd.openxmlformats-officedocument.wordprocessingml.document"; // Word 2007
    mime_map["dotx"] = "application/vnd.openxmlformats-officedocument.wordprocessingml.template"; // Word 2007 template
    mime_map["xlsx"] = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"; // Excel 2007
    mime_map["xltx"] = "application/vnd.openxmlformats-officedocument.spreadsheetml.template"; // Excel 2007 template
    mime_map["pptx"] = "application/vnd.openxmlformats-officedocument.presentationml.presentation"; // PowerPoint 2007 presentation
    mime_map["ppsx"] = "application/vnd.openxmlformats-officedocument.presentationml.slideshow"; // PowerPoint 2007 slideshow
    mime_map["potx"] = "application/vnd.openxmlformats-officedocument.presentationml.template"; // PowerPoint 2007 template
    mime_map["xps"] = "application/vnd.ms-xpsdocument";

    // Macro-enabled variants - these appear to be the same formats as the
    // above.  Currently we just treat them as the same mimetypes to avoid
    // having to check for twice as many possible content-types.
    // MS say: application/vnd.ms-word.document.macroEnabled.12
    mime_map["docm"] = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    // MS say: application/vnd.ms-word.template.macroEnabled.12
    mime_map["dotm"] = "application/vnd.openxmlformats-officedocument.wordprocessingml.template";
    // MS say: application/vnd.ms-excel.sheet.macroEnabled.12
    mime_map["xlsm"] = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    // MS say: application/vnd.ms-excel.template.macroEnabled.12
    mime_map["xltm"] = "application/vnd.openxmlformats-officedocument.spreadsheetml.template";
    // MS say: application/vnd.ms-powerpoint.presentation.macroEnabled.12
    mime_map["pptm"] = "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    // MS say: application/vnd.ms-powerpoint.slideshow.macroEnabled.12
    mime_map["ppsm"] = "application/vnd.openxmlformats-officedocument.presentationml.slideshow";
    // MS say: application/vnd.ms-powerpoint.presentation.macroEnabled.12
    mime_map["potm"] = "application/vnd.openxmlformats-officedocument.presentationml.template";

    // Some other word processor formats:
    mime_map["doc"] = "application/msword";
    mime_map["dot"] = "application/msword"; // Word template
    mime_map["wpd"] = "application/vnd.wordperfect";
    mime_map["wps"] = "application/vnd.ms-works";
    mime_map["wpt"] = "application/vnd.ms-works"; // Works template
    mime_map["abw"] = "application/x-abiword"; // AbiWord
    mime_map["zabw"] = "application/x-abiword-compressed"; // AbiWord compressed
    mime_map["rtf"] = "text/rtf";

    // Other MS formats:
    mime_map["xls"] = "application/vnd.ms-excel";
    mime_map["xlb"] = "application/vnd.ms-excel";
    mime_map["xlt"] = "application/vnd.ms-excel"; // Excel template
    mime_map["ppt"] = "application/vnd.ms-powerpoint";
    mime_map["pps"] = "application/vnd.ms-powerpoint"; // Powerpoint slideshow

    // Perl:
    mime_map["pl"] = "text/x-perl";
    mime_map["pm"] = "text/x-perl";
    mime_map["pod"] = "text/x-perl";

    // TeX DVI:
    mime_map["dvi"] = "application/x-dvi";

    // DjVu:
    mime_map["djv"] = "image/vnd.djvu";
    mime_map["djvu"] = "image/vnd.djvu";

    while ((getopt_ret = gnu_getopt_long(argc, argv, "hvd:D:U:M:l:s:pfS",
					 longopts, NULL)) != -1) {
	switch (getopt_ret) {
	case 'h': {
	    cout << PROG_NAME" - "PROG_DESC"\n\n"
"Usage: "PROG_NAME" [OPTIONS] --db DATABASE [BASEDIR] DIRECTORY\n\n"
"Options:\n"
"  -d, --duplicates         set duplicate handling ('ignore' or 'replace')\n"
"  -p, --preserve-nonduplicates  don't delete unupdated documents in\n"
"                                duplicate replace mode\n"
"  -D, --db                 path to database to use\n"
"  -U, --url                base url DIRECTORY represents (default: /)\n"
"  -M, --mime-type          additional MIME mapping ext:type\n"
"  -l, --depth-limit=LIMIT  set recursion limit (0 = unlimited)\n"
"  -f, --follow             follow symbolic links\n"
"  -S, --spelling           index data for spelling correction\n"
"      --overwrite          create the database anew (the default is to update\n"
"                           if the database already exists)" << endl;
	    print_stemmer_help("     ");
	    print_help_and_version_help("     ");
	    return 0;
	}
	case 'v':
	    print_package_info(PROG_NAME);
	    return 0;
	case 'd': // how shall we handle duplicate documents?
	    switch (optarg[0]) {
	    case 'i':
		skip_duplicates = true;
		break;
	    case 'r':
		skip_duplicates = false;
		break;
	    }
	    break;
	case 'p': // don't delete unupdated documents
	    preserve_unupdated = true;
	    break;
	case 'l': { // Set recursion limit
	    int arg = atoi(optarg);
	    if (arg < 0) arg = 0;
	    depth_limit = size_t(arg);
	    break;
	}
	case 'f': // Turn on following of symlinks
	    follow_symlinks = true;
	    break;
	case 'M': {
	    const char * s = strchr(optarg, ':');
	    if (s != NULL) {
		if (s[1]) {
		    mime_map[string(optarg, s - optarg)] = string(s + 1);
		} else {
		    // -Mtxt: removes the default mapping for .txt files.
		    mime_map.erase(string(optarg, s - optarg));
		}
	    } else {
		cerr << "Invalid MIME mapping '" << optarg << "'\n"
			"Should be of the form ext:type, eg txt:text/plain\n"
			"(or txt: to delete a default mapping)" << endl;
		return 1;
	    }
	    break;
	}
	case 'D':
	    dbpath = optarg;
	    break;
	case 'U':
	    baseurl = optarg;
	    break;
	case 'o': // --overwrite
	    overwrite = true;
	    break;
	case 's':
	    try {
		stemmer = Xapian::Stem(optarg);
	    } catch (const Xapian::InvalidArgumentError &) {
		cerr << "Unknown stemming language '" << optarg << "'.\n"
			"Available language names are: "
		     << Xapian::Stem::get_available_languages() << endl;
		return 1;
	    }
	    break;
	case 'S':
	    spelling = true;
	    break;
	case ':': // missing param
	    return 1;
	case '?': // unknown option: FIXME -> char
	    return 1;
	}
    }

    if (dbpath.empty()) {
	cerr << PROG_NAME": you must specify a database with --db." << endl;
	return 1;
    }
    if (baseurl.empty()) {
	cerr << PROG_NAME": --url not specified, assuming `/'." << endl;
    }
    // baseurl mustn't end '/' or you end up with the wrong URL
    // (//thing is different to /thing). We could probably make this
    // safe a different way, by ensuring that we don't put a leading '/'
    // on leafnames when scanning a directory, but this will do.
    if (!baseurl.empty() && baseurl[baseurl.length() - 1] == '/') {
	cout << "baseurl has trailing '/' ... removing ... " << endl;
	baseurl.resize(baseurl.size() - 1);
    }

    if (optind >= argc || optind + 2 < argc) {
	cerr << PROG_NAME": you must specify a directory to index.\n"
"Do this either as a single directory (corresponding to the base URL)\n"
"or two directories - the first corresponding to the base URL and the second\n"
"a subdirectory of that to index." << endl;
	return 1;
    }
    root = argv[optind];
    if (optind + 2 == argc) {
	indexroot = argv[optind + 1]; // relative to root
	if (indexroot.empty() || indexroot[0] != '/') {
	    indexroot = "/" + indexroot;
	}
    } else {
	indexroot = ""; // index the whole of root
    }

    int exitcode = 1;
    try {
	if (!overwrite) {
	    db = Xapian::WritableDatabase(dbpath, Xapian::DB_CREATE_OR_OPEN);
	    if (!skip_duplicates) {
		// + 1 so that db.get_lastdocid() is a valid subscript.
		updated.resize(db.get_lastdocid() + 1);
	    }
	    try {
		string ubound = db.get_value_upper_bound(VALUE_LASTMOD);
		if (!ubound.empty()) 
		    last_mod_max = binary_string_to_int(ubound);
	    } catch (const Xapian::UnimplementedError &) {
		numeric_limits<time_t> n;
		last_mod_max = n.max();
	    }
	} else {
	    db = Xapian::WritableDatabase(dbpath, Xapian::DB_CREATE_OR_OVERWRITE);
	}

	if (spelling) {
	    indexer.set_database(db);
	    indexer.set_flags(indexer.FLAG_SPELLING);
	}
	indexer.set_stemmer(stemmer);

	index_directory(depth_limit, "/", mime_map);
	if (!skip_duplicates && !preserve_unupdated) {
	    for (Xapian::docid did = 1; did < updated.size(); ++did) {
		if (!updated[did]) {
		    try {
			db.delete_document(did);
			cout << "Deleted document #" << did << endl;
		    } catch (const Xapian::DocNotFoundError &) {
		    }
		}
	    }
	}
	db.commit();
	// cout << "\n\nNow we have " << db.get_doccount() << " documents." << endl;
	exitcode = 0;
    } catch (const Xapian::Error &e) {
	cout << "Exception: " << e.get_msg() << endl;
    } catch (const string &s) {
	cout << "Exception: " << s << endl;
    } catch (const char *s) {
	cout << "Exception: " << s << endl;
    } catch (...) {
	cout << "Caught unknown exception" << endl;
    }

    // If we created a temporary directory then delete it.
    if (!tmpdir.empty()) rmdir(tmpdir.c_str());

    return exitcode;
}
