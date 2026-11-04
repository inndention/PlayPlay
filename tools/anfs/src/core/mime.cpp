// core/mime.cpp - content types without the npm packages (ANFS-F2a).
//
// The extension tables below are generated from the MIT-licensed npm packages mime-db 1.52.0, mime-types 2.1.35
// and mime 1.6.0 (see THIRD_PARTY_NOTICES.md at the repository root):
//   mime-db     Copyright (c) 2014 Jonathan Ong <me@jongleberry.com>
//               Copyright (c) 2015-2022 Douglas Christopher Wilson <doug@somethingdoug.com>
//   mime-types  Copyright (c) 2014 Jonathan Ong <me@jongleberry.com>
//               Copyright (c) 2015 Douglas Christopher Wilson <doug@somethingdoug.com>
//   mime        Copyright (c) 2010 Benjamin Thomas, Robert Kieffer
// Permission is hereby granted, free of charge, to any person obtaining a copy of these packages and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
// copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
// LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// Two tables, because Node uses two:
// - mime::forExtension / forPath = mime-types 2.1.35 lookup() (agent.js preview-media, server.patched.js:2515/2785):
//   the db is mime-db 1.52.0, the versions in bridge-src lan-fileserver package-lock.json;
// - mime::contentTypeForFile = what Express 4 res.sendFile / res.download put in Content-Type: `send` 0.19 with
//   mime 1.6.0 (lookup + charsets.lookup), whose older table differs for 131 extensions (wav is audio/wav there and
//   audio/wave in mime-types, exe is application/octet-stream, avif unknown).
// Lookup rules copied from the packages:
// - mime-types: ext = extname('x.' + input).toLowerCase().substr(1) (path.win32.extname), so "mp4", ".mp4" and a
//   full path all work, and ".bashrc" has no type;
// - mime 1.x: ext = input.replace(/^.*[\.\/\\]/, '').toLowerCase(): everything after the last '.', '/' or '\' (on
//   the first line), so a file named "json" without a dot is application/json, and an unknown one falls back;
//   charset "UTF-8" for /^text\/|^application\/(javascript|json)/.
// JS toLowerCase maps only A-Z and U+212A KELVIN SIGN onto the ASCII the keys are made of.
//
// The tables are generated, never hand-edited: node gen-mime.js <node_modules with mime-types 2.1.35, mime-db
// 1.52.0, mime 1.6.0> out.inc, where gen-mime.js sorts Object.keys(require('mime-types').types) and writes mime's
// types as differences (ANFS-F2a). test_anfs_core_fs compares
// both tables entry by entry with the packages when a local copy exists.
#include "core/mime.h"

#include "core/nodepath.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace anfs::mime {

namespace {

struct Entry {
    const char* ext;
    const char* type;
};

// mime-types 2.1.35 lookup table: mime-db 1.52.0 resolved with mime-types' source preference (1180 extensions).
const Entry kMimeTypes[] = {
    {"123", "application/vnd.lotus-1-2-3"}, {"1km", "application/vnd.1000minds.decision-model+xml"},
    {"3dml", "text/vnd.in3d.3dml"}, {"3ds", "image/x-3ds"}, {"3g2", "video/3gpp2"}, {"3gp", "video/3gpp"},
    {"3gpp", "video/3gpp"}, {"3mf", "model/3mf"}, {"7z", "application/x-7z-compressed"},
    {"aab", "application/x-authorware-bin"}, {"aac", "audio/x-aac"}, {"aam", "application/x-authorware-map"},
    {"aas", "application/x-authorware-seg"}, {"abw", "application/x-abiword"}, {"ac", "application/pkix-attr-cert"},
    {"acc", "application/vnd.americandynamics.acc"}, {"ace", "application/x-ace-compressed"},
    {"acu", "application/vnd.acucobol"}, {"acutc", "application/vnd.acucorp"}, {"adp", "audio/adpcm"},
    {"aep", "application/vnd.audiograph"}, {"afm", "application/x-font-type1"}, {"afp", "application/vnd.ibm.modcap"},
    {"age", "application/vnd.age"}, {"ahead", "application/vnd.ahead.space"}, {"ai", "application/postscript"},
    {"aif", "audio/x-aiff"}, {"aifc", "audio/x-aiff"}, {"aiff", "audio/x-aiff"},
    {"air", "application/vnd.adobe.air-application-installer-package+zip"}, {"ait", "application/vnd.dvb.ait"},
    {"ami", "application/vnd.amiga.ami"}, {"amr", "audio/amr"}, {"apk", "application/vnd.android.package-archive"},
    {"apng", "image/apng"}, {"appcache", "text/cache-manifest"}, {"application", "application/x-ms-application"},
    {"apr", "application/vnd.lotus-approach"}, {"arc", "application/x-freearc"}, {"arj", "application/x-arj"},
    {"asc", "application/pgp-keys"}, {"asf", "video/x-ms-asf"}, {"asm", "text/x-asm"},
    {"aso", "application/vnd.accpac.simply.aso"}, {"asx", "video/x-ms-asf"}, {"atc", "application/vnd.acucorp"},
    {"atom", "application/atom+xml"}, {"atomcat", "application/atomcat+xml"},
    {"atomdeleted", "application/atomdeleted+xml"}, {"atomsvc", "application/atomsvc+xml"},
    {"atx", "application/vnd.antix.game-component"}, {"au", "audio/basic"}, {"avci", "image/avci"},
    {"avcs", "image/avcs"}, {"avi", "video/x-msvideo"}, {"avif", "image/avif"}, {"aw", "application/applixware"},
    {"azf", "application/vnd.airzip.filesecure.azf"}, {"azs", "application/vnd.airzip.filesecure.azs"},
    {"azv", "image/vnd.airzip.accelerator.azv"}, {"azw", "application/vnd.amazon.ebook"}, {"b16", "image/vnd.pco.b16"},
    {"bat", "application/x-msdownload"}, {"bcpio", "application/x-bcpio"}, {"bdf", "application/x-font-bdf"},
    {"bdm", "application/vnd.syncml.dm+wbxml"}, {"bdoc", "application/bdoc"}, {"bed", "application/vnd.realvnc.bed"},
    {"bh2", "application/vnd.fujitsu.oasysprs"}, {"bin", "application/octet-stream"}, {"blb", "application/x-blorb"},
    {"blorb", "application/x-blorb"}, {"bmi", "application/vnd.bmi"}, {"bmml", "application/vnd.balsamiq.bmml+xml"},
    {"bmp", "image/bmp"}, {"book", "application/vnd.framemaker"}, {"box", "application/vnd.previewsystems.box"},
    {"boz", "application/x-bzip2"}, {"bpk", "application/octet-stream"},
    {"bsp", "model/vnd.valve.source.compiled-map"}, {"btif", "image/prs.btif"}, {"buffer", "application/octet-stream"},
    {"bz", "application/x-bzip"}, {"bz2", "application/x-bzip2"}, {"c", "text/x-c"},
    {"c11amc", "application/vnd.cluetrust.cartomobile-config"},
    {"c11amz", "application/vnd.cluetrust.cartomobile-config-pkg"}, {"c4d", "application/vnd.clonk.c4group"},
    {"c4f", "application/vnd.clonk.c4group"}, {"c4g", "application/vnd.clonk.c4group"},
    {"c4p", "application/vnd.clonk.c4group"}, {"c4u", "application/vnd.clonk.c4group"},
    {"cab", "application/vnd.ms-cab-compressed"}, {"caf", "audio/x-caf"}, {"cap", "application/vnd.tcpdump.pcap"},
    {"car", "application/vnd.curl.car"}, {"cat", "application/vnd.ms-pki.seccat"}, {"cb7", "application/x-cbr"},
    {"cba", "application/x-cbr"}, {"cbr", "application/x-cbr"}, {"cbt", "application/x-cbr"},
    {"cbz", "application/x-cbr"}, {"cc", "text/x-c"}, {"cco", "application/x-cocoa"},
    {"cct", "application/x-director"}, {"ccxml", "application/ccxml+xml"}, {"cdbcmsg", "application/vnd.contact.cmsg"},
    {"cdf", "application/x-netcdf"}, {"cdfx", "application/cdfx+xml"}, {"cdkey", "application/vnd.mediastation.cdkey"},
    {"cdmia", "application/cdmi-capability"}, {"cdmic", "application/cdmi-container"},
    {"cdmid", "application/cdmi-domain"}, {"cdmio", "application/cdmi-object"}, {"cdmiq", "application/cdmi-queue"},
    {"cdx", "chemical/x-cdx"}, {"cdxml", "application/vnd.chemdraw+xml"}, {"cdy", "application/vnd.cinderella"},
    {"cer", "application/pkix-cert"}, {"cfs", "application/x-cfs-compressed"}, {"cgm", "image/cgm"},
    {"chat", "application/x-chat"}, {"chm", "application/vnd.ms-htmlhelp"}, {"chrt", "application/vnd.kde.kchart"},
    {"cif", "chemical/x-cif"}, {"cii", "application/vnd.anser-web-certificate-issue-initiation"},
    {"cil", "application/vnd.ms-artgalry"}, {"cjs", "application/node"}, {"cla", "application/vnd.claymore"},
    {"class", "application/java-vm"}, {"clkk", "application/vnd.crick.clicker.keyboard"},
    {"clkp", "application/vnd.crick.clicker.palette"}, {"clkt", "application/vnd.crick.clicker.template"},
    {"clkw", "application/vnd.crick.clicker.wordbank"}, {"clkx", "application/vnd.crick.clicker"},
    {"clp", "application/x-msclip"}, {"cmc", "application/vnd.cosmocaller"}, {"cmdf", "chemical/x-cmdf"},
    {"cml", "chemical/x-cml"}, {"cmp", "application/vnd.yellowriver-custom-menu"}, {"cmx", "image/x-cmx"},
    {"cod", "application/vnd.rim.cod"}, {"coffee", "text/coffeescript"}, {"com", "application/x-msdownload"},
    {"conf", "text/plain"}, {"cpio", "application/x-cpio"}, {"cpl", "application/cpl+xml"}, {"cpp", "text/x-c"},
    {"cpt", "application/mac-compactpro"}, {"crd", "application/x-mscardfile"}, {"crl", "application/pkix-crl"},
    {"crt", "application/x-x509-ca-cert"}, {"crx", "application/x-chrome-extension"},
    {"cryptonote", "application/vnd.rig.cryptonote"}, {"csh", "application/x-csh"},
    {"csl", "application/vnd.citationstyles.style+xml"}, {"csml", "chemical/x-csml"},
    {"csp", "application/vnd.commonspace"}, {"css", "text/css"}, {"cst", "application/x-director"},
    {"csv", "text/csv"}, {"cu", "application/cu-seeme"}, {"curl", "text/vnd.curl"}, {"cww", "application/prs.cww"},
    {"cxt", "application/x-director"}, {"cxx", "text/x-c"}, {"dae", "model/vnd.collada+xml"},
    {"daf", "application/vnd.mobius.daf"}, {"dart", "application/vnd.dart"}, {"dataless", "application/vnd.fdsn.seed"},
    {"davmount", "application/davmount+xml"}, {"dbf", "application/vnd.dbf"}, {"dbk", "application/docbook+xml"},
    {"dcr", "application/x-director"}, {"dcurl", "text/vnd.curl.dcurl"}, {"dd2", "application/vnd.oma.dd2+xml"},
    {"ddd", "application/vnd.fujixerox.ddd"}, {"ddf", "application/vnd.syncml.dmddf+xml"}, {"dds", "image/vnd.ms-dds"},
    {"deb", "application/x-debian-package"}, {"def", "text/plain"}, {"deploy", "application/octet-stream"},
    {"der", "application/x-x509-ca-cert"}, {"dfac", "application/vnd.dreamfactory"},
    {"dgc", "application/x-dgc-compressed"}, {"dic", "text/x-c"}, {"dir", "application/x-director"},
    {"dis", "application/vnd.mobius.dis"}, {"disposition-notification", "message/disposition-notification"},
    {"dist", "application/octet-stream"}, {"distz", "application/octet-stream"}, {"djv", "image/vnd.djvu"},
    {"djvu", "image/vnd.djvu"}, {"dll", "application/x-msdownload"}, {"dmg", "application/x-apple-diskimage"},
    {"dmp", "application/vnd.tcpdump.pcap"}, {"dms", "application/octet-stream"}, {"dna", "application/vnd.dna"},
    {"doc", "application/msword"}, {"docm", "application/vnd.ms-word.document.macroenabled.12"},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"}, {"dot", "application/msword"},
    {"dotm", "application/vnd.ms-word.template.macroenabled.12"},
    {"dotx", "application/vnd.openxmlformats-officedocument.wordprocessingml.template"},
    {"dp", "application/vnd.osgi.dp"}, {"dpg", "application/vnd.dpgraph"}, {"dra", "audio/vnd.dra"},
    {"drle", "image/dicom-rle"}, {"dsc", "text/prs.lines.tag"}, {"dssc", "application/dssc+der"},
    {"dtb", "application/x-dtbook+xml"}, {"dtd", "application/xml-dtd"}, {"dts", "audio/vnd.dts"},
    {"dtshd", "audio/vnd.dts.hd"}, {"dump", "application/octet-stream"}, {"dvb", "video/vnd.dvb.file"},
    {"dvi", "application/x-dvi"}, {"dwd", "application/atsc-dwd+xml"}, {"dwf", "model/vnd.dwf"},
    {"dwg", "image/vnd.dwg"}, {"dxf", "image/vnd.dxf"}, {"dxp", "application/vnd.spotfire.dxp"},
    {"dxr", "application/x-director"}, {"ear", "application/java-archive"}, {"ecelp4800", "audio/vnd.nuera.ecelp4800"},
    {"ecelp7470", "audio/vnd.nuera.ecelp7470"}, {"ecelp9600", "audio/vnd.nuera.ecelp9600"},
    {"ecma", "application/ecmascript"}, {"edm", "application/vnd.novadigm.edm"},
    {"edx", "application/vnd.novadigm.edx"}, {"efif", "application/vnd.picsel"}, {"ei6", "application/vnd.pg.osasli"},
    {"elc", "application/octet-stream"}, {"emf", "image/emf"}, {"eml", "message/rfc822"},
    {"emma", "application/emma+xml"}, {"emotionml", "application/emotionml+xml"}, {"emz", "application/x-msmetafile"},
    {"eol", "audio/vnd.digital-winds"}, {"eot", "application/vnd.ms-fontobject"}, {"eps", "application/postscript"},
    {"epub", "application/epub+zip"}, {"es", "application/ecmascript"}, {"es3", "application/vnd.eszigno3+xml"},
    {"esa", "application/vnd.osgi.subsystem"}, {"esf", "application/vnd.epson.esf"},
    {"et3", "application/vnd.eszigno3+xml"}, {"etx", "text/x-setext"}, {"eva", "application/x-eva"},
    {"evy", "application/x-envoy"}, {"exe", "application/x-msdos-program"}, {"exi", "application/exi"},
    {"exp", "application/express"}, {"exr", "image/aces"}, {"ext", "application/vnd.novadigm.ext"},
    {"ez", "application/andrew-inset"}, {"ez2", "application/vnd.ezpix-album"},
    {"ez3", "application/vnd.ezpix-package"}, {"f", "text/x-fortran"}, {"f4v", "video/x-f4v"},
    {"f77", "text/x-fortran"}, {"f90", "text/x-fortran"}, {"fbs", "image/vnd.fastbidsheet"},
    {"fcdt", "application/vnd.adobe.formscentral.fcdt"}, {"fcs", "application/vnd.isac.fcs"},
    {"fdf", "application/vnd.fdf"}, {"fdt", "application/fdt+xml"},
    {"fe_launch", "application/vnd.denovo.fcselayout-link"}, {"fg5", "application/vnd.fujitsu.oasysgp"},
    {"fgd", "application/x-director"}, {"fh", "image/x-freehand"}, {"fh4", "image/x-freehand"},
    {"fh5", "image/x-freehand"}, {"fh7", "image/x-freehand"}, {"fhc", "image/x-freehand"},
    {"fig", "application/x-xfig"}, {"fits", "image/fits"}, {"flac", "audio/x-flac"}, {"fli", "video/x-fli"},
    {"flo", "application/vnd.micrografx.flo"}, {"flv", "video/x-flv"}, {"flw", "application/vnd.kde.kivio"},
    {"flx", "text/vnd.fmi.flexstor"}, {"fly", "text/vnd.fly"}, {"fm", "application/vnd.framemaker"},
    {"fnc", "application/vnd.frogans.fnc"}, {"fo", "application/vnd.software602.filler.form+xml"},
    {"for", "text/x-fortran"}, {"fpx", "image/vnd.fpx"}, {"frame", "application/vnd.framemaker"},
    {"fsc", "application/vnd.fsc.weblaunch"}, {"fst", "image/vnd.fst"}, {"ftc", "application/vnd.fluxtime.clip"},
    {"fti", "application/vnd.anser-web-funds-transfer-initiation"}, {"fvt", "video/vnd.fvt"},
    {"fxp", "application/vnd.adobe.fxp"}, {"fxpl", "application/vnd.adobe.fxp"}, {"fzs", "application/vnd.fuzzysheet"},
    {"g2w", "application/vnd.geoplan"}, {"g3", "image/g3fax"}, {"g3w", "application/vnd.geospace"},
    {"gac", "application/vnd.groove-account"}, {"gam", "application/x-tads"}, {"gbr", "application/rpki-ghostbusters"},
    {"gca", "application/x-gca-compressed"}, {"gdl", "model/vnd.gdl"},
    {"gdoc", "application/vnd.google-apps.document"}, {"ged", "text/vnd.familysearch.gedcom"},
    {"geo", "application/vnd.dynageo"}, {"geojson", "application/geo+json"},
    {"gex", "application/vnd.geometry-explorer"}, {"ggb", "application/vnd.geogebra.file"},
    {"ggt", "application/vnd.geogebra.tool"}, {"ghf", "application/vnd.groove-help"}, {"gif", "image/gif"},
    {"gim", "application/vnd.groove-identity-message"}, {"glb", "model/gltf-binary"}, {"gltf", "model/gltf+json"},
    {"gml", "application/gml+xml"}, {"gmx", "application/vnd.gmx"}, {"gnumeric", "application/x-gnumeric"},
    {"gph", "application/vnd.flographit"}, {"gpx", "application/gpx+xml"}, {"gqf", "application/vnd.grafeq"},
    {"gqs", "application/vnd.grafeq"}, {"gram", "application/srgs"}, {"gramps", "application/x-gramps-xml"},
    {"gre", "application/vnd.geometry-explorer"}, {"grv", "application/vnd.groove-injector"},
    {"grxml", "application/srgs+xml"}, {"gsf", "application/x-font-ghostscript"},
    {"gsheet", "application/vnd.google-apps.spreadsheet"}, {"gslides", "application/vnd.google-apps.presentation"},
    {"gtar", "application/x-gtar"}, {"gtm", "application/vnd.groove-tool-message"}, {"gtw", "model/vnd.gtw"},
    {"gv", "text/vnd.graphviz"}, {"gxf", "application/gxf"}, {"gxt", "application/vnd.geonext"},
    {"gz", "application/gzip"}, {"h", "text/x-c"}, {"h261", "video/h261"}, {"h263", "video/h263"},
    {"h264", "video/h264"}, {"hal", "application/vnd.hal+xml"}, {"hbci", "application/vnd.hbci"},
    {"hbs", "text/x-handlebars-template"}, {"hdd", "application/x-virtualbox-hdd"}, {"hdf", "application/x-hdf"},
    {"heic", "image/heic"}, {"heics", "image/heic-sequence"}, {"heif", "image/heif"}, {"heifs", "image/heif-sequence"},
    {"hej2", "image/hej2k"}, {"held", "application/atsc-held+xml"}, {"hh", "text/x-c"}, {"hjson", "application/hjson"},
    {"hlp", "application/winhlp"}, {"hpgl", "application/vnd.hp-hpgl"}, {"hpid", "application/vnd.hp-hpid"},
    {"hps", "application/vnd.hp-hps"}, {"hqx", "application/mac-binhex40"}, {"hsj2", "image/hsj2"},
    {"htc", "text/x-component"}, {"htke", "application/vnd.kenameaapp"}, {"htm", "text/html"}, {"html", "text/html"},
    {"hvd", "application/vnd.yamaha.hv-dic"}, {"hvp", "application/vnd.yamaha.hv-voice"},
    {"hvs", "application/vnd.yamaha.hv-script"}, {"i2g", "application/vnd.intergeo"},
    {"icc", "application/vnd.iccprofile"}, {"ice", "x-conference/x-cooltalk"}, {"icm", "application/vnd.iccprofile"},
    {"ico", "image/vnd.microsoft.icon"}, {"ics", "text/calendar"}, {"ief", "image/ief"}, {"ifb", "text/calendar"},
    {"ifm", "application/vnd.shana.informed.formdata"}, {"iges", "model/iges"}, {"igl", "application/vnd.igloader"},
    {"igm", "application/vnd.insors.igm"}, {"igs", "model/iges"}, {"igx", "application/vnd.micrografx.igx"},
    {"iif", "application/vnd.shana.informed.interchange"}, {"img", "application/octet-stream"},
    {"imp", "application/vnd.accpac.simply.imp"}, {"ims", "application/vnd.ms-ims"}, {"in", "text/plain"},
    {"ini", "text/plain"}, {"ink", "application/inkml+xml"}, {"inkml", "application/inkml+xml"},
    {"install", "application/x-install-instructions"}, {"iota", "application/vnd.astraea-software.iota"},
    {"ipfix", "application/ipfix"}, {"ipk", "application/vnd.shana.informed.package"},
    {"irm", "application/vnd.ibm.rights-management"}, {"irp", "application/vnd.irepository.package+xml"},
    {"iso", "application/x-iso9660-image"}, {"itp", "application/vnd.shana.informed.formtemplate"},
    {"its", "application/its+xml"}, {"ivp", "application/vnd.immervision-ivp"},
    {"ivu", "application/vnd.immervision-ivu"}, {"jad", "text/vnd.sun.j2me.app-descriptor"}, {"jade", "text/jade"},
    {"jam", "application/vnd.jam"}, {"jar", "application/java-archive"},
    {"jardiff", "application/x-java-archive-diff"}, {"java", "text/x-java-source"}, {"jhc", "image/jphc"},
    {"jisp", "application/vnd.jisp"}, {"jls", "image/jls"}, {"jlt", "application/vnd.hp-jlyt"}, {"jng", "image/x-jng"},
    {"jnlp", "application/x-java-jnlp-file"}, {"joda", "application/vnd.joost.joda-archive"}, {"jp2", "image/jp2"},
    {"jpe", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"jpf", "image/jpx"}, {"jpg", "image/jpeg"}, {"jpg2", "image/jp2"},
    {"jpgm", "video/jpm"}, {"jpgv", "video/jpeg"}, {"jph", "image/jph"}, {"jpm", "image/jpm"}, {"jpx", "image/jpx"},
    {"js", "application/javascript"}, {"json", "application/json"}, {"json5", "application/json5"},
    {"jsonld", "application/ld+json"}, {"jsonml", "application/jsonml+json"}, {"jsx", "text/jsx"},
    {"jxr", "image/jxr"}, {"jxra", "image/jxra"}, {"jxrs", "image/jxrs"}, {"jxs", "image/jxs"}, {"jxsc", "image/jxsc"},
    {"jxsi", "image/jxsi"}, {"jxss", "image/jxss"}, {"kar", "audio/midi"}, {"karbon", "application/vnd.kde.karbon"},
    {"kdbx", "application/x-keepass2"}, {"key", "application/vnd.apple.keynote"},
    {"kfo", "application/vnd.kde.kformula"}, {"kia", "application/vnd.kidspiration"},
    {"kml", "application/vnd.google-earth.kml+xml"}, {"kmz", "application/vnd.google-earth.kmz"},
    {"kne", "application/vnd.kinar"}, {"knp", "application/vnd.kinar"}, {"kon", "application/vnd.kde.kontour"},
    {"kpr", "application/vnd.kde.kpresenter"}, {"kpt", "application/vnd.kde.kpresenter"},
    {"kpxx", "application/vnd.ds-keypoint"}, {"ksp", "application/vnd.kde.kspread"},
    {"ktr", "application/vnd.kahootz"}, {"ktx", "image/ktx"}, {"ktx2", "image/ktx2"},
    {"ktz", "application/vnd.kahootz"}, {"kwd", "application/vnd.kde.kword"}, {"kwt", "application/vnd.kde.kword"},
    {"lasxml", "application/vnd.las.las+xml"}, {"latex", "application/x-latex"},
    {"lbd", "application/vnd.llamagraphics.life-balance.desktop"},
    {"lbe", "application/vnd.llamagraphics.life-balance.exchange+xml"}, {"les", "application/vnd.hhe.lesson-player"},
    {"less", "text/less"}, {"lgr", "application/lgr+xml"}, {"lha", "application/x-lzh-compressed"},
    {"link66", "application/vnd.route66.link66+xml"}, {"list", "text/plain"},
    {"list3820", "application/vnd.ibm.modcap"}, {"listafp", "application/vnd.ibm.modcap"},
    {"litcoffee", "text/coffeescript"}, {"lnk", "application/x-ms-shortcut"}, {"log", "text/plain"},
    {"lostxml", "application/lost+xml"}, {"lrf", "application/octet-stream"}, {"lrm", "application/vnd.ms-lrm"},
    {"ltf", "application/vnd.frogans.ltf"}, {"lua", "text/x-lua"}, {"luac", "application/x-lua-bytecode"},
    {"lvp", "audio/vnd.lucent.voice"}, {"lwp", "application/vnd.lotus-wordpro"},
    {"lzh", "application/x-lzh-compressed"}, {"m13", "application/x-msmediaview"},
    {"m14", "application/x-msmediaview"}, {"m1v", "video/mpeg"}, {"m21", "application/mp21"}, {"m2a", "audio/mpeg"},
    {"m2v", "video/mpeg"}, {"m3a", "audio/mpeg"}, {"m3u", "audio/x-mpegurl"},
    {"m3u8", "application/vnd.apple.mpegurl"}, {"m4a", "audio/mp4"}, {"m4p", "application/mp4"},
    {"m4s", "video/iso.segment"}, {"m4u", "video/vnd.mpegurl"}, {"m4v", "video/x-m4v"},
    {"ma", "application/mathematica"}, {"mads", "application/mads+xml"}, {"maei", "application/mmt-aei+xml"},
    {"mag", "application/vnd.ecowin.chart"}, {"maker", "application/vnd.framemaker"}, {"man", "text/troff"},
    {"manifest", "text/cache-manifest"}, {"map", "application/json"}, {"mar", "application/octet-stream"},
    {"markdown", "text/markdown"}, {"mathml", "application/mathml+xml"}, {"mb", "application/mathematica"},
    {"mbk", "application/vnd.mobius.mbk"}, {"mbox", "application/mbox"}, {"mc1", "application/vnd.medcalcdata"},
    {"mcd", "application/vnd.mcd"}, {"mcurl", "text/vnd.curl.mcurl"}, {"md", "text/markdown"},
    {"mdb", "application/x-msaccess"}, {"mdi", "image/vnd.ms-modi"}, {"mdx", "text/mdx"}, {"me", "text/troff"},
    {"mesh", "model/mesh"}, {"meta4", "application/metalink4+xml"}, {"metalink", "application/metalink+xml"},
    {"mets", "application/mets+xml"}, {"mfm", "application/vnd.mfmp"}, {"mft", "application/rpki-manifest"},
    {"mgp", "application/vnd.osgeo.mapguide.package"}, {"mgz", "application/vnd.proteus.magazine"},
    {"mid", "audio/midi"}, {"midi", "audio/midi"}, {"mie", "application/x-mie"}, {"mif", "application/vnd.mif"},
    {"mime", "message/rfc822"}, {"mj2", "video/mj2"}, {"mjp2", "video/mj2"}, {"mjs", "application/javascript"},
    {"mk3d", "video/x-matroska"}, {"mka", "audio/x-matroska"}, {"mkd", "text/x-markdown"}, {"mks", "video/x-matroska"},
    {"mkv", "video/x-matroska"}, {"mlp", "application/vnd.dolby.mlp"}, {"mmd", "application/vnd.chipnuts.karaoke-mmd"},
    {"mmf", "application/vnd.smaf"}, {"mml", "text/mathml"}, {"mmr", "image/vnd.fujixerox.edmics-mmr"},
    {"mng", "video/x-mng"}, {"mny", "application/x-msmoney"}, {"mobi", "application/x-mobipocket-ebook"},
    {"mods", "application/mods+xml"}, {"mov", "video/quicktime"}, {"movie", "video/x-sgi-movie"},
    {"mp2", "audio/mpeg"}, {"mp21", "application/mp21"}, {"mp2a", "audio/mpeg"}, {"mp3", "audio/mpeg"},
    {"mp4", "video/mp4"}, {"mp4a", "audio/mp4"}, {"mp4s", "application/mp4"}, {"mp4v", "video/mp4"},
    {"mpc", "application/vnd.mophun.certificate"}, {"mpd", "application/dash+xml"}, {"mpe", "video/mpeg"},
    {"mpeg", "video/mpeg"}, {"mpf", "application/media-policy-dataset+xml"}, {"mpg", "video/mpeg"},
    {"mpg4", "video/mp4"}, {"mpga", "audio/mpeg"}, {"mpkg", "application/vnd.apple.installer+xml"},
    {"mpm", "application/vnd.blueice.multipass"}, {"mpn", "application/vnd.mophun.application"},
    {"mpp", "application/dash-patch+xml"}, {"mpt", "application/vnd.ms-project"},
    {"mpy", "application/vnd.ibm.minipay"}, {"mqy", "application/vnd.mobius.mqy"}, {"mrc", "application/marc"},
    {"mrcx", "application/marcxml+xml"}, {"ms", "text/troff"}, {"mscml", "application/mediaservercontrol+xml"},
    {"mseed", "application/vnd.fdsn.mseed"}, {"mseq", "application/vnd.mseq"}, {"msf", "application/vnd.epson.msf"},
    {"msg", "application/vnd.ms-outlook"}, {"msh", "model/mesh"}, {"msi", "application/x-msdownload"},
    {"msl", "application/vnd.mobius.msl"}, {"msm", "application/octet-stream"}, {"msp", "application/octet-stream"},
    {"msty", "application/vnd.muvee.style"}, {"mtl", "model/mtl"}, {"mts", "model/vnd.mts"},
    {"mus", "application/vnd.musician"}, {"musd", "application/mmt-usd+xml"},
    {"musicxml", "application/vnd.recordare.musicxml+xml"}, {"mvb", "application/x-msmediaview"},
    {"mvt", "application/vnd.mapbox-vector-tile"}, {"mwf", "application/vnd.mfer"}, {"mxf", "application/mxf"},
    {"mxl", "application/vnd.recordare.musicxml"}, {"mxmf", "audio/mobile-xmf"}, {"mxml", "application/xv+xml"},
    {"mxs", "application/vnd.triscape.mxs"}, {"mxu", "video/vnd.mpegurl"},
    {"n-gage", "application/vnd.nokia.n-gage.symbian.install"}, {"n3", "text/n3"}, {"nb", "application/mathematica"},
    {"nbp", "application/vnd.wolfram.player"}, {"nc", "application/x-netcdf"}, {"ncx", "application/x-dtbncx+xml"},
    {"nfo", "text/x-nfo"}, {"ngdat", "application/vnd.nokia.n-gage.data"}, {"nitf", "application/vnd.nitf"},
    {"nlu", "application/vnd.neurolanguage.nlu"}, {"nml", "application/vnd.enliven"},
    {"nnd", "application/vnd.noblenet-directory"}, {"nns", "application/vnd.noblenet-sealer"},
    {"nnw", "application/vnd.noblenet-web"}, {"npx", "image/vnd.net-fpx"}, {"nq", "application/n-quads"},
    {"nsc", "application/x-conference"}, {"nsf", "application/vnd.lotus-notes"}, {"nt", "application/n-triples"},
    {"ntf", "application/vnd.nitf"}, {"numbers", "application/vnd.apple.numbers"}, {"nzb", "application/x-nzb"},
    {"oa2", "application/vnd.fujitsu.oasys2"}, {"oa3", "application/vnd.fujitsu.oasys3"},
    {"oas", "application/vnd.fujitsu.oasys"}, {"obd", "application/x-msbinder"},
    {"obgx", "application/vnd.openblox.game+xml"}, {"obj", "model/obj"}, {"oda", "application/oda"},
    {"odb", "application/vnd.oasis.opendocument.database"}, {"odc", "application/vnd.oasis.opendocument.chart"},
    {"odf", "application/vnd.oasis.opendocument.formula"},
    {"odft", "application/vnd.oasis.opendocument.formula-template"},
    {"odg", "application/vnd.oasis.opendocument.graphics"}, {"odi", "application/vnd.oasis.opendocument.image"},
    {"odm", "application/vnd.oasis.opendocument.text-master"},
    {"odp", "application/vnd.oasis.opendocument.presentation"},
    {"ods", "application/vnd.oasis.opendocument.spreadsheet"}, {"odt", "application/vnd.oasis.opendocument.text"},
    {"oga", "audio/ogg"}, {"ogex", "model/vnd.opengex"}, {"ogg", "audio/ogg"}, {"ogv", "video/ogg"},
    {"ogx", "application/ogg"}, {"omdoc", "application/omdoc+xml"}, {"onepkg", "application/onenote"},
    {"onetmp", "application/onenote"}, {"onetoc", "application/onenote"}, {"onetoc2", "application/onenote"},
    {"opf", "application/oebps-package+xml"}, {"opml", "text/x-opml"}, {"oprc", "application/vnd.palm"},
    {"opus", "audio/ogg"}, {"org", "application/vnd.lotus-organizer"},
    {"osf", "application/vnd.yamaha.openscoreformat"}, {"osfpvg", "application/vnd.yamaha.openscoreformat.osfpvg+xml"},
    {"osm", "application/vnd.openstreetmap.data+xml"}, {"otc", "application/vnd.oasis.opendocument.chart-template"},
    {"otf", "font/otf"}, {"otg", "application/vnd.oasis.opendocument.graphics-template"},
    {"oth", "application/vnd.oasis.opendocument.text-web"},
    {"oti", "application/vnd.oasis.opendocument.image-template"},
    {"otp", "application/vnd.oasis.opendocument.presentation-template"},
    {"ots", "application/vnd.oasis.opendocument.spreadsheet-template"},
    {"ott", "application/vnd.oasis.opendocument.text-template"}, {"ova", "application/x-virtualbox-ova"},
    {"ovf", "application/x-virtualbox-ovf"}, {"owl", "application/rdf+xml"}, {"oxps", "application/oxps"},
    {"oxt", "application/vnd.openofficeorg.extension"}, {"p", "text/x-pascal"}, {"p10", "application/pkcs10"},
    {"p12", "application/x-pkcs12"}, {"p7b", "application/x-pkcs7-certificates"}, {"p7c", "application/pkcs7-mime"},
    {"p7m", "application/pkcs7-mime"}, {"p7r", "application/x-pkcs7-certreqresp"},
    {"p7s", "application/pkcs7-signature"}, {"p8", "application/pkcs8"}, {"pac", "application/x-ns-proxy-autoconfig"},
    {"pages", "application/vnd.apple.pages"}, {"pas", "text/x-pascal"}, {"paw", "application/vnd.pawaafile"},
    {"pbd", "application/vnd.powerbuilder6"}, {"pbm", "image/x-portable-bitmap"},
    {"pcap", "application/vnd.tcpdump.pcap"}, {"pcf", "application/x-font-pcf"}, {"pcl", "application/vnd.hp-pcl"},
    {"pclxl", "application/vnd.hp-pclxl"}, {"pct", "image/x-pict"}, {"pcurl", "application/vnd.curl.pcurl"},
    {"pcx", "image/vnd.zbrush.pcx"}, {"pdb", "application/vnd.palm"}, {"pde", "text/x-processing"},
    {"pdf", "application/pdf"}, {"pem", "application/x-x509-ca-cert"}, {"pfa", "application/x-font-type1"},
    {"pfb", "application/x-font-type1"}, {"pfm", "application/x-font-type1"}, {"pfr", "application/font-tdpfr"},
    {"pfx", "application/x-pkcs12"}, {"pgm", "image/x-portable-graymap"}, {"pgn", "application/x-chess-pgn"},
    {"pgp", "application/pgp-encrypted"}, {"php", "application/x-httpd-php"}, {"pic", "image/x-pict"},
    {"pkg", "application/octet-stream"}, {"pki", "application/pkixcmp"}, {"pkipath", "application/pkix-pkipath"},
    {"pkpass", "application/vnd.apple.pkpass"}, {"pl", "application/x-perl"},
    {"plb", "application/vnd.3gpp.pic-bw-large"}, {"plc", "application/vnd.mobius.plc"},
    {"plf", "application/vnd.pocketlearn"}, {"pls", "application/pls+xml"}, {"pm", "application/x-perl"},
    {"pml", "application/vnd.ctc-posml"}, {"png", "image/png"}, {"pnm", "image/x-portable-anymap"},
    {"portpkg", "application/vnd.macports.portpkg"}, {"pot", "application/vnd.ms-powerpoint"},
    {"potm", "application/vnd.ms-powerpoint.template.macroenabled.12"},
    {"potx", "application/vnd.openxmlformats-officedocument.presentationml.template"},
    {"ppam", "application/vnd.ms-powerpoint.addin.macroenabled.12"}, {"ppd", "application/vnd.cups-ppd"},
    {"ppm", "image/x-portable-pixmap"}, {"pps", "application/vnd.ms-powerpoint"},
    {"ppsm", "application/vnd.ms-powerpoint.slideshow.macroenabled.12"},
    {"ppsx", "application/vnd.openxmlformats-officedocument.presentationml.slideshow"},
    {"ppt", "application/vnd.ms-powerpoint"}, {"pptm", "application/vnd.ms-powerpoint.presentation.macroenabled.12"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"pqa", "application/vnd.palm"}, {"prc", "application/x-mobipocket-ebook"},
    {"pre", "application/vnd.lotus-freelance"}, {"prf", "application/pics-rules"},
    {"provx", "application/provenance+xml"}, {"ps", "application/postscript"},
    {"psb", "application/vnd.3gpp.pic-bw-small"}, {"psd", "image/vnd.adobe.photoshop"},
    {"psf", "application/x-font-linux-psf"}, {"pskcxml", "application/pskc+xml"}, {"pti", "image/prs.pti"},
    {"ptid", "application/vnd.pvi.ptid1"}, {"pub", "application/x-mspublisher"},
    {"pvb", "application/vnd.3gpp.pic-bw-var"}, {"pwn", "application/vnd.3m.post-it-notes"},
    {"pya", "audio/vnd.ms-playready.media.pya"}, {"pyv", "video/vnd.ms-playready.media.pyv"},
    {"qam", "application/vnd.epson.quickanime"}, {"qbo", "application/vnd.intu.qbo"},
    {"qfx", "application/vnd.intu.qfx"}, {"qps", "application/vnd.publishare-delta-tree"}, {"qt", "video/quicktime"},
    {"qwd", "application/vnd.quark.quarkxpress"}, {"qwt", "application/vnd.quark.quarkxpress"},
    {"qxb", "application/vnd.quark.quarkxpress"}, {"qxd", "application/vnd.quark.quarkxpress"},
    {"qxl", "application/vnd.quark.quarkxpress"}, {"qxt", "application/vnd.quark.quarkxpress"},
    {"ra", "audio/x-pn-realaudio"}, {"ram", "audio/x-pn-realaudio"}, {"raml", "application/raml+yaml"},
    {"rapd", "application/route-apd+xml"}, {"rar", "application/vnd.rar"}, {"ras", "image/x-cmu-raster"},
    {"rcprofile", "application/vnd.ipunplugged.rcprofile"}, {"rdf", "application/rdf+xml"},
    {"rdz", "application/vnd.data-vision.rdz"}, {"relo", "application/p2p-overlay+xml"},
    {"rep", "application/vnd.businessobjects"}, {"res", "application/x-dtbresource+xml"}, {"rgb", "image/x-rgb"},
    {"rif", "application/reginfo+xml"}, {"rip", "audio/vnd.rip"}, {"ris", "application/x-research-info-systems"},
    {"rl", "application/resource-lists+xml"}, {"rlc", "image/vnd.fujixerox.edmics-rlc"},
    {"rld", "application/resource-lists-diff+xml"}, {"rm", "application/vnd.rn-realmedia"}, {"rmi", "audio/midi"},
    {"rmp", "audio/x-pn-realaudio-plugin"}, {"rms", "application/vnd.jcp.javame.midlet-rms"},
    {"rmvb", "application/vnd.rn-realmedia-vbr"}, {"rnc", "application/relax-ng-compact-syntax"},
    {"rng", "application/xml"}, {"roa", "application/rpki-roa"}, {"roff", "text/troff"},
    {"rp9", "application/vnd.cloanto.rp9"}, {"rpm", "application/x-redhat-package-manager"},
    {"rpss", "application/vnd.nokia.radio-presets"}, {"rpst", "application/vnd.nokia.radio-preset"},
    {"rq", "application/sparql-query"}, {"rs", "application/rls-services+xml"}, {"rsat", "application/atsc-rsat+xml"},
    {"rsd", "application/rsd+xml"}, {"rsheet", "application/urc-ressheet+xml"}, {"rss", "application/rss+xml"},
    {"rtf", "application/rtf"}, {"rtx", "text/richtext"}, {"run", "application/x-makeself"},
    {"rusd", "application/route-usd+xml"}, {"s", "text/x-asm"}, {"s3m", "audio/s3m"},
    {"saf", "application/vnd.yamaha.smaf-audio"}, {"sass", "text/x-sass"}, {"sbml", "application/sbml+xml"},
    {"sc", "application/vnd.ibm.secure-container"}, {"scd", "application/x-msschedule"},
    {"scm", "application/vnd.lotus-screencam"}, {"scq", "application/scvp-cv-request"},
    {"scs", "application/scvp-cv-response"}, {"scss", "text/x-scss"}, {"scurl", "text/vnd.curl.scurl"},
    {"sda", "application/vnd.stardivision.draw"}, {"sdc", "application/vnd.stardivision.calc"},
    {"sdd", "application/vnd.stardivision.impress"}, {"sdkd", "application/vnd.solent.sdkm+xml"},
    {"sdkm", "application/vnd.solent.sdkm+xml"}, {"sdp", "application/sdp"},
    {"sdw", "application/vnd.stardivision.writer"}, {"sea", "application/x-sea"}, {"see", "application/vnd.seemail"},
    {"seed", "application/vnd.fdsn.seed"}, {"sema", "application/vnd.sema"}, {"semd", "application/vnd.semd"},
    {"semf", "application/vnd.semf"}, {"senmlx", "application/senml+xml"}, {"sensmlx", "application/sensml+xml"},
    {"ser", "application/java-serialized-object"}, {"setpay", "application/set-payment-initiation"},
    {"setreg", "application/set-registration-initiation"}, {"sfd-hdstx", "application/vnd.hydrostatix.sof-data"},
    {"sfs", "application/vnd.spotfire.sfs"}, {"sfv", "text/x-sfv"}, {"sgi", "image/sgi"},
    {"sgl", "application/vnd.stardivision.writer-global"}, {"sgm", "text/sgml"}, {"sgml", "text/sgml"},
    {"sh", "application/x-sh"}, {"shar", "application/x-shar"}, {"shex", "text/shex"}, {"shf", "application/shf+xml"},
    {"shtml", "text/html"}, {"sid", "image/x-mrsid-image"}, {"sieve", "application/sieve"},
    {"sig", "application/pgp-signature"}, {"sil", "audio/silk"}, {"silo", "model/mesh"},
    {"sis", "application/vnd.symbian.install"}, {"sisx", "application/vnd.symbian.install"},
    {"sit", "application/x-stuffit"}, {"sitx", "application/x-stuffitx"}, {"siv", "application/sieve"},
    {"skd", "application/vnd.koan"}, {"skm", "application/vnd.koan"}, {"skp", "application/vnd.koan"},
    {"skt", "application/vnd.koan"}, {"sldm", "application/vnd.ms-powerpoint.slide.macroenabled.12"},
    {"sldx", "application/vnd.openxmlformats-officedocument.presentationml.slide"}, {"slim", "text/slim"},
    {"slm", "text/slim"}, {"sls", "application/route-s-tsid+xml"}, {"slt", "application/vnd.epson.salt"},
    {"sm", "application/vnd.stepmania.stepchart"}, {"smf", "application/vnd.stardivision.math"},
    {"smi", "application/smil+xml"}, {"smil", "application/smil+xml"}, {"smv", "video/x-smv"},
    {"smzip", "application/vnd.stepmania.package"}, {"snd", "audio/basic"}, {"snf", "application/x-font-snf"},
    {"so", "application/octet-stream"}, {"spc", "application/x-pkcs7-certificates"}, {"spdx", "text/spdx"},
    {"spf", "application/vnd.yamaha.smaf-phrase"}, {"spl", "application/x-futuresplash"},
    {"spot", "text/vnd.in3d.spot"}, {"spp", "application/scvp-vp-response"}, {"spq", "application/scvp-vp-request"},
    {"spx", "audio/ogg"}, {"sql", "application/x-sql"}, {"src", "application/x-wais-source"},
    {"srt", "application/x-subrip"}, {"sru", "application/sru+xml"}, {"srx", "application/sparql-results+xml"},
    {"ssdl", "application/ssdl+xml"}, {"sse", "application/vnd.kodak-descriptor"},
    {"ssf", "application/vnd.epson.ssf"}, {"ssml", "application/ssml+xml"},
    {"st", "application/vnd.sailingtracker.track"}, {"stc", "application/vnd.sun.xml.calc.template"},
    {"std", "application/vnd.sun.xml.draw.template"}, {"stf", "application/vnd.wt.stf"},
    {"sti", "application/vnd.sun.xml.impress.template"}, {"stk", "application/hyperstudio"}, {"stl", "model/stl"},
    {"stpx", "model/step+xml"}, {"stpxz", "model/step-xml+zip"}, {"stpz", "model/step+zip"},
    {"str", "application/vnd.pg.format"}, {"stw", "application/vnd.sun.xml.writer.template"}, {"styl", "text/stylus"},
    {"stylus", "text/stylus"}, {"sub", "text/vnd.dvb.subtitle"}, {"sus", "application/vnd.sus-calendar"},
    {"susp", "application/vnd.sus-calendar"}, {"sv4cpio", "application/x-sv4cpio"}, {"sv4crc", "application/x-sv4crc"},
    {"svc", "application/vnd.dvb.service"}, {"svd", "application/vnd.svd"}, {"svg", "image/svg+xml"},
    {"svgz", "image/svg+xml"}, {"swa", "application/x-director"}, {"swf", "application/x-shockwave-flash"},
    {"swi", "application/vnd.aristanetworks.swi"}, {"swidtag", "application/swid+xml"},
    {"sxc", "application/vnd.sun.xml.calc"}, {"sxd", "application/vnd.sun.xml.draw"},
    {"sxg", "application/vnd.sun.xml.writer.global"}, {"sxi", "application/vnd.sun.xml.impress"},
    {"sxm", "application/vnd.sun.xml.math"}, {"sxw", "application/vnd.sun.xml.writer"}, {"t", "text/troff"},
    {"t3", "application/x-t3vm-image"}, {"t38", "image/t38"}, {"taglet", "application/vnd.mynfc"},
    {"tao", "application/vnd.tao.intent-module-archive"}, {"tap", "image/vnd.tencent.tap"},
    {"tar", "application/x-tar"}, {"tcap", "application/vnd.3gpp2.tcap"}, {"tcl", "application/x-tcl"},
    {"td", "application/urc-targetdesc+xml"}, {"teacher", "application/vnd.smart.teacher"},
    {"tei", "application/tei+xml"}, {"teicorpus", "application/tei+xml"}, {"tex", "application/x-tex"},
    {"texi", "application/x-texinfo"}, {"texinfo", "application/x-texinfo"}, {"text", "text/plain"},
    {"tfi", "application/thraud+xml"}, {"tfm", "application/x-tex-tfm"}, {"tfx", "image/tiff-fx"},
    {"tga", "image/x-tga"}, {"thmx", "application/vnd.ms-officetheme"}, {"tif", "image/tiff"}, {"tiff", "image/tiff"},
    {"tk", "application/x-tcl"}, {"tmo", "application/vnd.tmobile-livetv"}, {"toml", "application/toml"},
    {"torrent", "application/x-bittorrent"}, {"tpl", "application/vnd.groove-tool-template"},
    {"tpt", "application/vnd.trid.tpt"}, {"tr", "text/troff"}, {"tra", "application/vnd.trueapp"},
    {"trig", "application/trig"}, {"trm", "application/x-msterminal"}, {"ts", "video/mp2t"},
    {"tsd", "application/timestamped-data"}, {"tsv", "text/tab-separated-values"}, {"ttc", "font/collection"},
    {"ttf", "font/ttf"}, {"ttl", "text/turtle"}, {"ttml", "application/ttml+xml"},
    {"twd", "application/vnd.simtech-mindmapper"}, {"twds", "application/vnd.simtech-mindmapper"},
    {"txd", "application/vnd.genomatix.tuxedo"}, {"txf", "application/vnd.mobius.txf"}, {"txt", "text/plain"},
    {"u32", "application/x-authorware-bin"}, {"u8dsn", "message/global-delivery-status"},
    {"u8hdr", "message/global-headers"}, {"u8mdn", "message/global-disposition-notification"},
    {"u8msg", "message/global"}, {"ubj", "application/ubjson"}, {"udeb", "application/x-debian-package"},
    {"ufd", "application/vnd.ufdl"}, {"ufdl", "application/vnd.ufdl"}, {"ulx", "application/x-glulx"},
    {"umj", "application/vnd.umajin"}, {"unityweb", "application/vnd.unity"}, {"uoml", "application/vnd.uoml+xml"},
    {"uri", "text/uri-list"}, {"uris", "text/uri-list"}, {"urls", "text/uri-list"}, {"usdz", "model/vnd.usdz+zip"},
    {"ustar", "application/x-ustar"}, {"utz", "application/vnd.uiq.theme"}, {"uu", "text/x-uuencode"},
    {"uva", "audio/vnd.dece.audio"}, {"uvd", "application/vnd.dece.data"}, {"uvf", "application/vnd.dece.data"},
    {"uvg", "image/vnd.dece.graphic"}, {"uvh", "video/vnd.dece.hd"}, {"uvi", "image/vnd.dece.graphic"},
    {"uvm", "video/vnd.dece.mobile"}, {"uvp", "video/vnd.dece.pd"}, {"uvs", "video/vnd.dece.sd"},
    {"uvt", "application/vnd.dece.ttml+xml"}, {"uvu", "video/vnd.uvvu.mp4"}, {"uvv", "video/vnd.dece.video"},
    {"uvva", "audio/vnd.dece.audio"}, {"uvvd", "application/vnd.dece.data"}, {"uvvf", "application/vnd.dece.data"},
    {"uvvg", "image/vnd.dece.graphic"}, {"uvvh", "video/vnd.dece.hd"}, {"uvvi", "image/vnd.dece.graphic"},
    {"uvvm", "video/vnd.dece.mobile"}, {"uvvp", "video/vnd.dece.pd"}, {"uvvs", "video/vnd.dece.sd"},
    {"uvvt", "application/vnd.dece.ttml+xml"}, {"uvvu", "video/vnd.uvvu.mp4"}, {"uvvv", "video/vnd.dece.video"},
    {"uvvx", "application/vnd.dece.unspecified"}, {"uvvz", "application/vnd.dece.zip"},
    {"uvx", "application/vnd.dece.unspecified"}, {"uvz", "application/vnd.dece.zip"},
    {"vbox", "application/x-virtualbox-vbox"}, {"vbox-extpack", "application/x-virtualbox-vbox-extpack"},
    {"vcard", "text/vcard"}, {"vcd", "application/x-cdlink"}, {"vcf", "text/x-vcard"},
    {"vcg", "application/vnd.groove-vcard"}, {"vcs", "text/x-vcalendar"}, {"vcx", "application/vnd.vcx"},
    {"vdi", "application/x-virtualbox-vdi"}, {"vds", "model/vnd.sap.vds"}, {"vhd", "application/x-virtualbox-vhd"},
    {"vis", "application/vnd.visionary"}, {"viv", "video/vnd.vivo"}, {"vmdk", "application/x-virtualbox-vmdk"},
    {"vob", "video/x-ms-vob"}, {"vor", "application/vnd.stardivision.writer"}, {"vox", "application/x-authorware-bin"},
    {"vrml", "model/vrml"}, {"vsd", "application/vnd.visio"}, {"vsf", "application/vnd.vsf"},
    {"vss", "application/vnd.visio"}, {"vst", "application/vnd.visio"}, {"vsw", "application/vnd.visio"},
    {"vtf", "image/vnd.valve.source.texture"}, {"vtt", "text/vtt"}, {"vtu", "model/vnd.vtu"},
    {"vxml", "application/voicexml+xml"}, {"w3d", "application/x-director"}, {"wad", "application/x-doom"},
    {"wadl", "application/vnd.sun.wadl+xml"}, {"war", "application/java-archive"}, {"wasm", "application/wasm"},
    {"wav", "audio/wave"}, {"wax", "audio/x-ms-wax"}, {"wbmp", "image/vnd.wap.wbmp"},
    {"wbs", "application/vnd.criticaltools.wbs+xml"}, {"wbxml", "application/vnd.wap.wbxml"},
    {"wcm", "application/vnd.ms-works"}, {"wdb", "application/vnd.ms-works"}, {"wdp", "image/vnd.ms-photo"},
    {"weba", "audio/webm"}, {"webapp", "application/x-web-app-manifest+json"}, {"webm", "video/webm"},
    {"webmanifest", "application/manifest+json"}, {"webp", "image/webp"}, {"wg", "application/vnd.pmi.widget"},
    {"wgt", "application/widget"}, {"wif", "application/watcherinfo+xml"}, {"wks", "application/vnd.ms-works"},
    {"wm", "video/x-ms-wm"}, {"wma", "audio/x-ms-wma"}, {"wmd", "application/x-ms-wmd"}, {"wmf", "image/wmf"},
    {"wml", "text/vnd.wap.wml"}, {"wmlc", "application/vnd.wap.wmlc"}, {"wmls", "text/vnd.wap.wmlscript"},
    {"wmlsc", "application/vnd.wap.wmlscriptc"}, {"wmv", "video/x-ms-wmv"}, {"wmx", "video/x-ms-wmx"},
    {"wmz", "application/x-ms-wmz"}, {"woff", "font/woff"}, {"woff2", "font/woff2"},
    {"wpd", "application/vnd.wordperfect"}, {"wpl", "application/vnd.ms-wpl"}, {"wps", "application/vnd.ms-works"},
    {"wqd", "application/vnd.wqd"}, {"wri", "application/x-mswrite"}, {"wrl", "model/vrml"},
    {"wsc", "message/vnd.wfa.wsc"}, {"wsdl", "application/wsdl+xml"}, {"wspolicy", "application/wspolicy+xml"},
    {"wtb", "application/vnd.webturbo"}, {"wvx", "video/x-ms-wvx"}, {"x32", "application/x-authorware-bin"},
    {"x3d", "model/x3d+xml"}, {"x3db", "model/x3d+fastinfoset"}, {"x3dbz", "model/x3d+binary"},
    {"x3dv", "model/x3d-vrml"}, {"x3dvz", "model/x3d+vrml"}, {"x3dz", "model/x3d+xml"},
    {"x_b", "model/vnd.parasolid.transmit.binary"}, {"x_t", "model/vnd.parasolid.transmit.text"},
    {"xaml", "application/xaml+xml"}, {"xap", "application/x-silverlight-app"}, {"xar", "application/vnd.xara"},
    {"xav", "application/xcap-att+xml"}, {"xbap", "application/x-ms-xbap"},
    {"xbd", "application/vnd.fujixerox.docuworks.binder"}, {"xbm", "image/x-xbitmap"},
    {"xca", "application/xcap-caps+xml"}, {"xcs", "application/calendar+xml"}, {"xdf", "application/xcap-diff+xml"},
    {"xdm", "application/vnd.syncml.dm+xml"}, {"xdp", "application/vnd.adobe.xdp+xml"},
    {"xdssc", "application/dssc+xml"}, {"xdw", "application/vnd.fujixerox.docuworks"},
    {"xel", "application/xcap-el+xml"}, {"xenc", "application/xenc+xml"}, {"xer", "application/patch-ops-error+xml"},
    {"xfdf", "application/vnd.adobe.xfdf"}, {"xfdl", "application/vnd.xfdl"}, {"xht", "application/xhtml+xml"},
    {"xhtml", "application/xhtml+xml"}, {"xhvml", "application/xv+xml"}, {"xif", "image/vnd.xiff"},
    {"xla", "application/vnd.ms-excel"}, {"xlam", "application/vnd.ms-excel.addin.macroenabled.12"},
    {"xlc", "application/vnd.ms-excel"}, {"xlf", "application/xliff+xml"}, {"xlm", "application/vnd.ms-excel"},
    {"xls", "application/vnd.ms-excel"}, {"xlsb", "application/vnd.ms-excel.sheet.binary.macroenabled.12"},
    {"xlsm", "application/vnd.ms-excel.sheet.macroenabled.12"},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"}, {"xlt", "application/vnd.ms-excel"},
    {"xltm", "application/vnd.ms-excel.template.macroenabled.12"},
    {"xltx", "application/vnd.openxmlformats-officedocument.spreadsheetml.template"},
    {"xlw", "application/vnd.ms-excel"}, {"xm", "audio/xm"}, {"xml", "application/xml"},
    {"xns", "application/xcap-ns+xml"}, {"xo", "application/vnd.olpc-sugar"}, {"xop", "application/xop+xml"},
    {"xpi", "application/x-xpinstall"}, {"xpl", "application/xproc+xml"}, {"xpm", "image/x-xpixmap"},
    {"xpr", "application/vnd.is-xpr"}, {"xps", "application/vnd.ms-xpsdocument"},
    {"xpw", "application/vnd.intercon.formnet"}, {"xpx", "application/vnd.intercon.formnet"},
    {"xsd", "application/xml"}, {"xsl", "application/xml"}, {"xslt", "application/xslt+xml"},
    {"xsm", "application/vnd.syncml+xml"}, {"xspf", "application/xspf+xml"},
    {"xul", "application/vnd.mozilla.xul+xml"}, {"xvm", "application/xv+xml"}, {"xvml", "application/xv+xml"},
    {"xwd", "image/x-xwindowdump"}, {"xyz", "chemical/x-xyz"}, {"xz", "application/x-xz"}, {"yaml", "text/yaml"},
    {"yang", "application/yang"}, {"yin", "application/yin+xml"}, {"yml", "text/yaml"}, {"ymp", "text/x-suse-ymp"},
    {"z1", "application/x-zmachine"}, {"z2", "application/x-zmachine"}, {"z3", "application/x-zmachine"},
    {"z4", "application/x-zmachine"}, {"z5", "application/x-zmachine"}, {"z6", "application/x-zmachine"},
    {"z7", "application/x-zmachine"}, {"z8", "application/x-zmachine"}, {"zaz", "application/vnd.zzazz.deck+xml"},
    {"zip", "application/zip"}, {"zir", "application/vnd.zul"}, {"zirz", "application/vnd.zul"},
    {"zmm", "application/vnd.handheld-entertainment+xml"},
};

// mime 1.6.0 (Express 4 send) as differences from kMimeTypes: a different type, or "" = unknown to mime 1.6.0
// (1069 extensions in all).
const Entry kMime1Delta[] = {
    {"1km", ""}, {"3mf", ""}, {"age", ""}, {"amr", ""}, {"asc", "application/pgp-signature"}, {"atomdeleted", ""},
    {"avci", ""}, {"avcs", ""}, {"avif", ""}, {"azv", ""}, {"b16", ""}, {"bmml", ""}, {"bsp", ""}, {"cdfx", ""},
    {"cjs", ""}, {"cpl", ""}, {"csl", ""}, {"dbf", ""}, {"ddf", ""}, {"dds", ""}, {"deb", "application/octet-stream"},
    {"disposition-notification", ""}, {"dll", "application/octet-stream"}, {"dmg", "application/octet-stream"},
    {"drle", ""}, {"dwd", ""}, {"emf", "application/x-msmetafile"}, {"emotionml", ""}, {"es", ""},
    {"exe", "application/octet-stream"}, {"exp", ""}, {"exr", ""}, {"fdt", ""}, {"fits", ""}, {"fo", ""}, {"ged", ""},
    {"heic", ""}, {"heics", ""}, {"heif", ""}, {"heifs", ""}, {"hej2", ""}, {"held", ""}, {"hjson", "text/hjson"},
    {"hsj2", ""}, {"ico", "image/x-icon"}, {"iso", "application/octet-stream"}, {"its", ""}, {"jhc", ""}, {"jls", ""},
    {"jph", ""}, {"jxr", ""}, {"jxra", ""}, {"jxrs", ""}, {"jxs", ""}, {"jxsc", ""}, {"jxsi", ""}, {"jxss", ""},
    {"kdbx", ""}, {"key", ""}, {"ktx2", ""}, {"lgr", ""}, {"m4s", ""}, {"maei", ""}, {"mdx", ""}, {"mpf", ""},
    {"mpp", "application/vnd.ms-project"}, {"msi", "application/octet-stream"}, {"mtl", ""}, {"musd", ""}, {"mvt", ""},
    {"mxmf", ""}, {"nq", ""}, {"nt", ""}, {"numbers", ""}, {"obgx", ""}, {"obj", "application/x-tgif"}, {"ogex", ""},
    {"opus", ""}, {"osm", ""}, {"owl", ""}, {"pages", ""}, {"pcx", "image/x-pcx"}, {"provx", ""}, {"pti", ""},
    {"rapd", ""}, {"rar", "application/x-rar-compressed"}, {"relo", ""}, {"rsat", ""}, {"rsheet", ""}, {"rusd", ""},
    {"senmlx", ""}, {"sensmlx", ""}, {"shex", ""}, {"sieve", ""}, {"siv", ""}, {"sls", ""}, {"spdx", ""},
    {"stl", "application/vnd.ms-pki.stl"}, {"stpx", ""}, {"stpxz", ""}, {"stpz", ""}, {"swidtag", ""}, {"t38", ""},
    {"tap", ""}, {"td", ""}, {"tfx", ""}, {"toml", ""}, {"trig", ""}, {"ttml", ""}, {"u8dsn", ""}, {"u8hdr", ""},
    {"u8mdn", ""}, {"u8msg", ""}, {"ubj", ""}, {"usdz", ""}, {"vds", ""}, {"vtf", ""}, {"wav", "audio/wav"},
    {"wif", ""}, {"wmf", "application/x-msmetafile"}, {"wsc", ""}, {"x3db", "model/x3d+binary"},
    {"x3dv", "model/x3d+vrml"}, {"x_b", ""}, {"x_t", ""}, {"xav", ""}, {"xca", ""}, {"xcs", ""}, {"xel", ""},
    {"xlf", "application/x-xliff+xml"}, {"xns", ""},
};

template <size_t N>
const Entry* find(const Entry (&table)[N], const std::string& ext)
{
    const Entry* end = table + N;
    const Entry* it = std::lower_bound(table, end, ext, [](const Entry& e, const std::string& k) {
        return std::strcmp(e.ext, k.c_str()) < 0;
    });
    return it != end && ext == it->ext ? it : nullptr;
}

// JS String.prototype.toLowerCase as far as it can produce a table key: A-Z, and U+212A (E2 84 AA) -> 'k'.
std::string jsLowerKey(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c + 32));
        } else if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x84 &&
                   static_cast<unsigned char>(s[i + 2]) == 0xAA) {
            out.push_back('k');
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string lookupMimeTypes(const std::string& ext)
{
    if (ext.empty()) return {};
    const Entry* e = find(kMimeTypes, ext);
    return e ? std::string(e->type) : std::string();
}

std::string lookupMime1(const std::string& ext)
{
    if (ext.empty()) return {};
    if (const Entry* d = find(kMime1Delta, ext)) return d->type;
    return lookupMimeTypes(ext);
}

// The line terminators JS's '.' does not match: \n, \r, U+2028, U+2029.
size_t firstLineEnd(std::string_view s)
{
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n' || c == '\r') return i;
        if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(s[i + 2]) == 0xA8 || static_cast<unsigned char>(s[i + 2]) == 0xA9))
            return i;
    }
    return s.size();
}

} // namespace

std::string forExtension(std::string_view ext)
{
    // mime-types: extname('x.' + path).toLowerCase().substr(1)
    std::string probe = "x.";
    probe += ext;
    const std::string e = nodepath::extname(probe);
    if (e.size() < 2) return {};
    return lookupMimeTypes(jsLowerKey(std::string_view(e).substr(1)));
}

std::string forPath(std::string_view path, std::string_view fallback)
{
    std::string t = forExtension(nodepath::extname(path));
    return t.empty() ? std::string(fallback) : t;
}

std::string contentTypeForFile(std::string_view path, std::string_view fallback)
{
    // mime 1.6 lookup: path.replace(/^.*[\.\/\\]/, '') keeps what follows the last '.', '/' or '\' of the first line.
    const size_t lineEnd = firstLineEnd(path);
    size_t cut = 0;
    for (size_t i = lineEnd; i-- > 0;)
        if (path[i] == '.' || path[i] == '/' || path[i] == '\\') {
            cut = i + 1;
            break;
        }
    std::string type = lookupMime1(jsLowerKey(path.substr(cut)));
    if (type.empty()) type = std::string(fallback);
    if (type.empty()) return {};
    // mime.charsets.lookup: (/^text\/|^application\/(javascript|json)/).test(type) ? 'UTF-8' : fallback
    if (type.compare(0, 5, "text/") == 0 || type.compare(0, 22, "application/javascript") == 0 ||
        type.compare(0, 16, "application/json") == 0)
        type += "; charset=UTF-8";
    return type;
}

} // namespace anfs::mime
