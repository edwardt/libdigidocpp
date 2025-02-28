/*
 * libdigidocpp
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "ASiContainer.h"

#include "DataFile_p.h"
#include "log.h"
#include "Signature.h"
#include "util/DateTime.h"
#include "util/File.h"
#include "util/ZipSerialize.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

using namespace digidoc;
using namespace digidoc::util;
using namespace std;

#define MAX_MEM_FILE 500*1024*1024

class ASiContainer::Private
{
public:
    string mimetype, path;
    vector<DataFile*> documents;
    vector<Signature*> signatures;
    map<string, ZipSerialize::Properties> properties;
};

const string ASiContainer::ASICE_EXTENSION = "asice";
const string ASiContainer::ASICE_EXTENSION_ABBR = "sce";
const string ASiContainer::ASICS_EXTENSION = "asics";
const string ASiContainer::ASICS_EXTENSION_ABBR = "scs";
const string ASiContainer::BDOC_EXTENSION = "bdoc";

const string ASiContainer::MIMETYPE_ASIC_E = "application/vnd.etsi.asic-e+zip";
const string ASiContainer::MIMETYPE_ASIC_S = "application/vnd.etsi.asic-s+zip";
//https://signa.mitsoft.lt/static/signa-web/webResources/docs/ADOC_specification_approved20090907_EN.pdf
const string ASiContainer::MIMETYPE_ADOC = "application/vnd.lt.archyvai.adoc-2008";

/**
 * Initialize BDOC container.
 */
ASiContainer::ASiContainer(const string &mimetype)
 : d(new Private)
{
     d->mimetype = mimetype;
}

/**
 * Loads ASi Container from a file.
 *
 * @param path name of the container file.
 * @param mimetypeRequired flag indicating if the mimetype must be present and checked.
 * @param supported supported mimetypes.
 * @return returns zip serializer for the container.
 */
unique_ptr<ZipSerialize> ASiContainer::load(const string &path, bool mimetypeRequired, const set<string> &supported)
{
    DEBUG("ASiContainer::ASiContainer(path = '%s')", path.c_str());
    unique_ptr<ZipSerialize> z( new ZipSerialize(d->path = path, false) );

    vector<string> list = z->list();
    if(list.empty())
        THROW("Failed to parse container");

    for(const string &file: list)
        d->properties[file] = z->properties(file);

    if(mimetypeRequired && list[0] != "mimetype")
        THROW("required mimetype not found");

    // ETSI TS 102 918: mimetype has to be the first in the archive;
    if(list[0] == "mimetype")
    {
        stringstream data;
        z->extract(list.front(), data);
        d->mimetype = readMimetype(data);
        DEBUG("mimetype = '%s'", d->mimetype.c_str());
        if(supported.find(d->mimetype) == supported.cend())
            THROW("Incorrect mimetype '%s'", d->mimetype.c_str());
    }

    return z;
}

string ASiContainer::mediaType() const
{
    return d->mimetype;
}

/**
 * Releases resources.
 */
ASiContainer::~ASiContainer()
{
    for_each(d->signatures.cbegin(), d->signatures.cend(), default_delete<Signature>());
    for_each(d->documents.cbegin(), d->documents.cend(), default_delete<DataFile>());
    delete d;
}

/**
 * Returns document referenced by document id.
 *
 * @return returns dataFiles.
 */
vector<DataFile*> ASiContainer::dataFiles() const
{
    return d->documents;
}

/**
 * Returns signature referenced by signature id.
 *
 * @return returns signature referenced by signature id.
 * @throws ContainerException throws exception if the signature id is incorrect.
 */
vector<Signature *> ASiContainer::signatures() const
{
    return d->signatures;
}

/**
 * <p>
 * Read a datafile from container.
 * </p>
 * If expected size of the data is too big, then stream is written to temp file.
 *
 * @param path name of the file in zip container stream is used to read from.
 * @param z Zip container.
 * @return returns data as a stream.
 */
unique_ptr<iostream> ASiContainer::dataStream(const string &path, const ZipSerialize &z) const
{
    unique_ptr<iostream> data;
    if(d->properties[path].size > MAX_MEM_FILE)
        data.reset(new fstream(File::encodeName(File::tempFileName()).c_str(), fstream::in|fstream::out|fstream::binary|fstream::trunc));
    else
        data.reset(new stringstream);
    z.extract(path, *data);
    return data;
}

/**
 * Adds document to the container. Documents can be removed from container only
 * after all signatures are removed.
 *
 * @param path path of a document, which is added to the container.
 * @param mediaType Mimetype of the file
 * @throws ContainerException exception is thrown if the document path is incorrect or document
 *         with same file name already exists. Also no document can be added if the
 *         container already has one or more signatures.
 */
void ASiContainer::addDataFile(const string &path, const string &mediaType)
{
    string fileName = File::fileName(path);
    addDataFileChecks(fileName, mediaType);
    if(!File::fileExists(path))
        THROW("Document file '%s' does not exist.", path.c_str());

    ZipSerialize::Properties prop = { appInfo(), File::modifiedTime(path), File::fileSize(path) };
    zproperty(File::fileName(path), prop);
    unique_ptr<istream> is;
    if(prop.size > MAX_MEM_FILE)
    {
        is.reset(new ifstream(File::encodeName(path).c_str(), ifstream::binary));
    }
    else
    {
        ifstream file(File::encodeName(path).c_str(), ifstream::binary);
        stringstream *data = new stringstream;
        if(file)
            *data << file.rdbuf();
        file.close();
        is.reset(data);
    }
    addDataFilePrivate(move(is), fileName, mediaType);
}

void ASiContainer::addDataFile(unique_ptr<istream> is, const string &fileName, const string &mediaType)
{
    addDataFileChecks(fileName, mediaType);
    if(fileName.find_last_of("/\\") != string::npos)
        THROW("Document file '%s' cannot contain directory path.", fileName.c_str());
    addDataFilePrivate(move(is), fileName, mediaType);
}

void ASiContainer::addDataFileChecks(const string &fileName, const string &mediaType)
{
    if(!d->signatures.empty())
        THROW("Can not add document to container which has signatures, remove all signatures before adding new document.");
    if(fileName == "mimetype")
        THROW("mimetype is reserved file.");
    if(any_of(d->documents.cbegin(), d->documents.cend(), [&](DataFile *file) { return fileName == file->fileName(); }))
        THROW("Document with same file name '%s' already exists.", fileName.c_str());
    if(mediaType.find('/') == string::npos)
        THROW("MediaType does not meet format requirements (RFC2045, section 5.1) '%s'.", mediaType.c_str());
}

void ASiContainer::addDataFilePrivate(unique_ptr<istream> is, const string &fileName, const string &mediaType)
{
    d->documents.push_back(new DataFilePrivate(move(is), fileName, mediaType));
}

/**
 * Removes document from container by document id. Documents can be
 * removed from container only after all signatures are removed.
 *
 * @param id document's id, which will be removed.
 * @throws ContainerException throws exception if the document id is incorrect or there are
 *         one or more signatures.
 */
void ASiContainer::removeDataFile(unsigned int id)
{
    if(!d->signatures.empty())
        THROW("Can not remove document from container which has signatures, remove all signatures before removing document.");
    if(id >= d->documents.size())
        THROW("Incorrect document id %u, there are only %lu documents in container.", id, (unsigned long)dataFiles().size());
    vector<DataFile*>::const_iterator it = (d->documents.cbegin() + id);
    delete *it;
    d->documents.erase(it);
}

void ASiContainer::addSignature(Signature *signature)
{
    d->signatures.push_back(signature);
}


/**
 * Removes signature from container by signature id.
 *
 * @param id signature's id, which will be removed.
 * @throws ContainerException throws exception if the signature id is incorrect.
 */
void ASiContainer::removeSignature(unsigned int id)
{
    if(id >= d->signatures.size())
        THROW("Incorrect signature id %u, there are only %lu signatures in container.", id, (unsigned long)d->signatures.size());
    vector<Signature*>::const_iterator it = (d->signatures.cbegin() + id);
    delete *it;
    d->signatures.erase(it);
}

void ASiContainer::deleteSignature(Signature* s)
{
    vector<Signature*>::const_iterator i = find(d->signatures.cbegin(), d->signatures.cend(), s);
    if(i != d->signatures.cend())
        d->signatures.erase(i);
    delete s;
}

void ASiContainer::zpath(const string &file)
{
    d->path = file;
}

string ASiContainer::zpath() const
{
    return d->path;
}

ZipSerialize::Properties ASiContainer::zproperty(const string &file) const
{
    map<string, ZipSerialize::Properties>::const_iterator i = d->properties.find(file);
    if(i != d->properties.cend())
        return i->second;
    return d->properties[file] = { appInfo(), date::gmtime(time(nullptr)), 0 };
}

void ASiContainer::zproperty(const string &file, const ZipSerialize::Properties &prop)
{
    d->properties[file] = prop;
}

/**
 * Reads and parses container mimetype. Checks that the mimetype is supported.
 *
 * @param path path to container directory.
 * @throws IOException exception is thrown if there was error reading mimetype file from disk.
 * @throws ContainerException exception is thrown if the parsed mimetype is incorrect.
 */
string ASiContainer::readMimetype(istream &is)
{
    DEBUG("ASiContainer::readMimetype()");
    unsigned char bom[] = { 0, 0, 0 };
    is.read((char*)bom, sizeof(bom));
    // Contains UTF-16 BOM
    if((bom[0] == 0xFF && bom[1] == 0xEF) ||
       (bom[0] == 0xEF && bom[1] == 0xFF))
        THROW("Mimetype file must be UTF-8 format.");
    // does not contain UTF-8 BOM reset pos
    if(!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
        is.seekg(0, ios::beg);

    string text;
    is >> text;
    if(is.fail())
        THROW("Failed to read mimetype.");

    return text;
}
