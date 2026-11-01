/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "plugin.h"

#include "iscriplib.h"
#include "ibrush.h"
#include "ipatch.h"
#include "ifiletypes.h"
#include "ieclass.h"
#include "qerplugin.h"
#include "eclasslib.h"
#include "ientity.h"
#include "layers.h"
#include "script/scripttokeniser.h"
#include "stream/stringstream.h"

#include "scenelib.h"
#include "string/string.h"
#include "stringio.h"
#include "generic/constant.h"

#include "modulesystem/singletonmodule.h"

#include "imap.h"

#include <kvpp/kvpp.h>

#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

// copied from plugins/mapq3/parse.cpp
class LayersParser
{
	Layers& m_layers;
	std::vector<Layer::iterator> m_layersVec; // vector to access layers by index
	std::map<int32_t, Layer*> m_visgroupidMap;
	int m_currentLayer = LAYERIDX0;
public:
	LayersParser( scene::Node& root ) : m_layers( *Node_getLayers( root ) ){
	}
	void construct_tree(){
		{
			auto& list = m_layers.m_children;

#if 0
			if( list.size() > 1 ) // something was parsed, remove default inserted layer
				list.pop_front();
#endif

			m_layersVec.reserve( list.size() );
			for( auto it = list.begin(); it != list.end(); ++it )
				m_layersVec.push_back( it );
			// check safety // note possible circular reference is not checked
			if( std::ranges::find( list, LAYERIDXPARENT, &Layer::m_parentIndex ) == list.cend() ){
				globalErrorStream() << "No layers in the root\n";
				list.front().m_parentIndex = LAYERIDXPARENT;
			}
		}
		for( auto it : m_layersVec ){
			// check safety
			if( it->m_parentIndex < LAYERIDXPARENT || it->m_parentIndex >= static_cast<int>( m_layersVec.size() ) ){
				globalErrorStream() << it->m_parentIndex << " parent layer index out of bounds\n";
				it->m_parentIndex = LAYERIDXPARENT;
			}
			if( it->m_parentIndex > LAYERIDXPARENT && m_layersVec[ it->m_parentIndex ] == it ){
				globalErrorStream() << it->m_parentIndex << " parent layer index self reference\n";
				it->m_parentIndex = LAYERIDXPARENT;
			}
			// link & reference parent
			Layer& parent = it->m_parentIndex == LAYERIDXPARENT? m_layers : *m_layersVec[ it->m_parentIndex ];
			parent.m_children.splice( parent.m_children.cend(), m_layers.m_children, it );
			it->m_parent = &parent;
		}

		m_layers.m_currentLayer = &m_layers.m_children.front();
	}
	bool read_layers( const kvpp::KV1ElementReadable<std::string_view>& visgroups ){
		for ( const auto& visgroup : visgroups ) {
			int32_t visgroupid = visgroup["visgroupid"].getValue<int32_t>();
			// CBRE-EX marks its automatic cast/trigger visgroups with negative ids
			if ( visgroupid < 0 ) {
				continue;
			}
			auto& layer = m_layers.m_children.emplace_back( visgroup["name"].getValue().data(), nullptr );
			layer.m_parentIndex = LAYERIDXPARENT;
			std::string color( visgroup["color"].getValue() );
			sscanf( color.c_str(), "%d %d %d", &layer.m_color[ 0 ], &layer.m_color[ 1 ], &layer.m_color[ 2 ]);
			m_visgroupidMap[visgroupid] = &layer;
		}
		return true;
	}
	Layer* from_visgroupid(int32_t visgroupid) {
		return m_visgroupidMap[visgroupid];
	}
	Layer* getCurrentLayer(){
		return m_layersVec[ m_currentLayer ].operator->();
	}
	Layer* getFirstlayer(){
		return &m_layers.m_children.front();
	}
};

inline MapImporter* Node_getMapImporter( scene::Node& node ){
	return NodeTypeCast<MapImporter>::cast( node );
}

inline MapExporter* Node_getMapExporter( scene::Node& node ){
	return NodeTypeCast<MapExporter>::cast( node );
}

NodeSmartReference g_nullNode( NewNullNode() );

class MapDependencies :
	public GlobalRadiantModuleRef,
	public GlobalBrushModuleRef,
	public GlobalPatchModuleRef,
	public GlobalFiletypesModuleRef,
	public GlobalScripLibModuleRef,
	public GlobalEntityClassManagerModuleRef,
	public GlobalSceneGraphModuleRef
{
public:
	MapDependencies() :
		GlobalBrushModuleRef( GlobalRadiant().getRequiredGameDescriptionKeyValue( "brushtypes" ) ),
		GlobalPatchModuleRef( GlobalRadiant().getRequiredGameDescriptionKeyValue( "patchtypes" ) ),
		GlobalEntityClassManagerModuleRef( GlobalRadiant().getRequiredGameDescriptionKeyValue( "entityclass" ) ){
	}
};

class MapVMFAPI final : public TypeSystemRef, public MapFormat
{
public:
	typedef MapFormat Type;
	STRING_CONSTANT( Name, "mapvmf" );

	MapVMFAPI(){
		GlobalFiletypesModule::getTable().addType( Type::Name, Name, filetype_t( "vmf maps", "*.vmf" ) );
		GlobalFiletypesModule::getTable().addType( Type::Name, Name, filetype_t( "vmf backups", "*.vmx" ) );
		GlobalFiletypesModule::getTable().addType( Type::Name, Name, filetype_t( "vmf region", "*.reg" ) );
	}
	MapFormat* getTable(){
		return this;
	}
	static bool key_is( std::string_view key, const char* name ){
		const std::size_t length = string_length( name );
		return key.size() == length && string_equal_nocase_n( key.data(), name, length );
	}
	static bool entity_is_group( const Entity* entity ){
		return entity != nullptr && entity->hasKeyValue( "_cbre_group" );
	}
	void readGraph( scene::Node& root, TextInputStream& inputStream, EntityCreator& entityTable ) const override {

		int mapversion = 0;
		char buffer[2048];
		size_t len = 0;
		std::string kv1Data = "";

		// FIXME: should this be hardcoded?
		GlobalBrushCreator().toggleFormat(eBrushTypeValveSource);

		while ( ( len = inputStream.read(buffer, sizeof(buffer)) ) > 0 ) {
			kv1Data.append(buffer, len);
		}

		kvpp::KV1 kv1(kv1Data);

		LayersParser layersParser( root );

		std::map<int64_t, scene::Node*> groupNodes;
		std::vector<NodeSmartReference> groupRefs;

		const auto find_group = [&groupNodes]( int64_t id ) -> scene::Node* {
			const auto it = groupNodes.find( id );
			return it != groupNodes.end() ? it->second : nullptr;
		};

		const auto read_solid = [&]( const kvpp::KV1ElementReadable<std::string_view>& e, scene::Node& parent ){
			scene::Node& solid( GlobalBrushCreator().createBrush() );
			solid.m_layer = nullptr;
			int64_t groupid = 0;
			for ( const auto& solidelem : e ) {
				if ( key_is( solidelem.getKey(), "side" ) ) {
					float uvaxis[2][4]{};
					float scale[2]{ 1.0f, 1.0f };
					const std::string uaxis( solidelem["uaxis"].getValue() );
					const std::string vaxis( solidelem["vaxis"].getValue() );
					sscanf(uaxis.c_str(), "[%f %f %f %f] %f", &uvaxis[0][0], &uvaxis[0][1], &uvaxis[0][2], &uvaxis[0][3], &scale[0]);
					sscanf(vaxis.c_str(), "[%f %f %f %f] %f", &uvaxis[1][0], &uvaxis[1][1], &uvaxis[1][2], &uvaxis[1][3], &scale[1]);

					std::string lightmapscale( solidelem["lightmapscale"].getValue() );
					if ( lightmapscale.empty() ) {
						lightmapscale = "16";
					}

					std::string faceData = std::format("{} {} [{} {} {} {}] [{} {} {} {}] {} {} {} {}\n}}\n",
						solidelem["plane"].getValue(), solidelem["material"].getValue(),
						uvaxis[0][0], uvaxis[0][1], uvaxis[0][2], uvaxis[0][3],
						uvaxis[1][0], uvaxis[1][1], uvaxis[1][2], uvaxis[1][3],
						solidelem["rotation"].getValue(),
						scale[0], scale[1],
						lightmapscale
					);

					MapImporter* importer = Node_getMapImporter( solid );
					StringInputStream istream( faceData );
					importer->importTokens( NewScriptTokeniser( istream ) );
				} else if ( key_is( solidelem.getKey(), "editor" ) ) {
					if ( solidelem.hasChild("visgroupid") ) {
						int32_t visgroupid = solidelem["visgroupid"].getValue<int32_t>();
						solid.m_layer = layersParser.from_visgroupid(visgroupid);
					}
					if ( solidelem.hasChild("groupid") ) {
						groupid = solidelem["groupid"].getValue<int64_t>();
					}
				}
			}
			if ( !solid.m_layer ) {
				solid.m_layer = layersParser.getFirstlayer();
			}
			scene::Node* target = &parent;
			if ( groupid > 0 ) {
				if ( scene::Node* group = find_group( groupid ) ) {
					target = group;
				}
			}
			NodeSmartReference solidnode( solid );
			if ( !Node_getTraversable( *target ) ) {
				globalWarningStream() << "FIXME: bad solid data in " << Node_getEntity( parent )->getClassName() << '\n';
			} else {
				Node_getTraversable( *target )->insert( solidnode );
			}
		};

		for ( const auto& elem : kv1 ) {
			const auto key = elem.getKey();
			if (key_is(key, "versioninfo")) {
				for ( const auto& e : elem ) {
					if (key_is(e.getKey(), "mapversion")) {
						mapversion = e.getValue<int>();
					}
				}
			} else if (key_is(key, "cameras")) {
				// FIXME: do we care about any of this?
			} else if (key_is(key, "cordon")) {
				// FIXME: do we care about any of this?
			} else if (key_is(key, "visgroups")) {
				layersParser.read_layers( elem );
			} else if (key_is(key, "world") || key_is(key, "entity")) {
				const bool isWorld = key_is(key, "world");
				bool hasSolids = isWorld;
				EntityClass* entityClass = NULL;
				for ( const auto& e : elem ) {
					if (key_is(e.getKey(), "solid")) {
						if ( e.hasChild("side") ) {
							hasSolids = true;
						}
					} else if (key_is(e.getKey(), "hidden")) {
						for ( const auto& hidden : e ) {
							if (key_is(hidden.getKey(), "solid") && hidden.hasChild("side")) {
								hasSolids = true;
							}
						}
					}
				}
				for ( const auto& e : elem ) {
					if (key_is(e.getKey(), "classname")) {
						entityClass = GlobalEntityClassManager().findOrInsert( e.getValue().data(), hasSolids );
					}
				}
				if ( !entityClass ) {
					globalWarningStream() << "no classname in entity" << '\n';
					continue;
				}
				scene::Node& entity( entityTable.createEntity( entityClass ) );
				entity.m_layer = nullptr;

				if ( isWorld ) {
					std::vector<std::pair<scene::Node*, int64_t>> pendingGroups;
					for ( const auto& e : elem ) {
						if ( !key_is(e.getKey(), "group") ) {
							continue;
						}
						const int64_t groupid = e.hasChild("id") ? e["id"].getValue<int64_t>() : 0;
						int64_t parentGroup = 0;
						if ( e.hasChild("editor") && e["editor"].hasChild("groupid") ) {
							parentGroup = e["editor"]["groupid"].getValue<int64_t>();
						}
						EntityClass* groupClass = GlobalEntityClassManager().findOrInsert( "func_group", true );
						scene::Node& groupNode( entityTable.createEntity( groupClass ) );
						Node_getEntity( groupNode )->setKeyValue( "_cbre_group", "1" );
						groupNode.m_layer = layersParser.getFirstlayer();
						if ( e.hasChild("editor") && e["editor"].hasChild("visgroupid") ) {
							groupNode.m_layer = layersParser.from_visgroupid( e["editor"]["visgroupid"].getValue<int32_t>() );
							if ( !groupNode.m_layer ) {
								groupNode.m_layer = layersParser.getFirstlayer();
							}
						}
						groupRefs.emplace_back( groupNode );
						groupNodes[groupid] = &groupNode;
						pendingGroups.emplace_back( &groupNode, parentGroup );
					}

					std::set<scene::Node*> placedGroups;
					while ( !pendingGroups.empty() ) {
						bool progress = false;
						std::vector<std::pair<scene::Node*, int64_t>> remaining;
						for ( const auto& pending : pendingGroups ) {
							scene::Node* parentNode = &entity;
							if ( pending.second > 0 ) {
								if ( scene::Node* group = find_group( pending.second ) ) {
									if ( group != pending.first ) {
										parentNode = group;
									}
								}
							}
							if ( parentNode == &entity || placedGroups.contains( parentNode ) ) {
								Node_getTraversable( *parentNode )->insert( NodeSmartReference( *pending.first ) );
								placedGroups.insert( pending.first );
								progress = true;
							} else {
								remaining.push_back( pending );
							}
						}
						if ( !progress ) {
							for ( const auto& pending : remaining ) {
								Node_getTraversable( entity )->insert( NodeSmartReference( *pending.first ) );
							}
							break;
						}
						pendingGroups = std::move( remaining );
					}
				}

				int64_t entityGroupId = 0;
				for ( const auto& e : elem ) {
					if (key_is(e.getKey(), "id")) {
						// FIXME: do we need to keep track of this?
					} else if (key_is(e.getKey(), "group")) {
						// already handled above
					} else if (key_is(e.getKey(), "hidden")) {
						for ( const auto& hidden : e ) {
							if (key_is(hidden.getKey(), "solid")) {
								read_solid( hidden, entity );
							}
						}
					} else if (key_is(e.getKey(), "solid")) {
						if ( !e.hasChild("side") ) {
							Node_getEntity( entity )->setKeyValue( e.getKey().data(), e.getValue().data() );
							continue;
						}
						read_solid( e, entity );
					} else if (key_is(e.getKey(), "editor")) {
						if ( e.hasChild("visgroupid") ) {
							int32_t visgroupid = e["visgroupid"].getValue<int32_t>();
							entity.m_layer = layersParser.from_visgroupid(visgroupid);
						}
						if ( e.hasChild("groupid") ) {
							entityGroupId = e["groupid"].getValue<int64_t>();
						}
					} else if (key_is(e.getKey(), "connections")) {
						for ( const auto& connection : e ) {
							Node_getEntity( entity )->addOutput( connection.getKey().data(), connection.getValue().data() );
						}
					} else {
						if (hasSolids && key_is(e.getKey(), "origin")) {
							// do nothing
						} else {
							Node_getEntity( entity )->setKeyValue( e.getKey().data(), e.getValue().data() );
						}
					}
				}
				if ( !entity.m_layer ) {
					entity.m_layer = layersParser.getFirstlayer();
				}
				scene::Node* insertParent = &root;
				if ( !isWorld && entityGroupId > 0 ) {
					if ( scene::Node* group = find_group( entityGroupId ) ) {
						insertParent = group;
					}
				}
				NodeSmartReference node( entity );
				Node_getTraversable( *insertParent )->insert( node );
			}
		}

		GlobalRadiant().Map_setVersion( mapversion );
		layersParser.construct_tree();
	}
	class MapVMFWriteKeyValue : public Entity::Visitor
	{
		int64_t& m_childID;
		kvpp::KV1ElementWritable<std::string>& m_element;
	public:
		MapVMFWriteKeyValue( kvpp::KV1ElementWritable<std::string>& element, int64_t& childID ) : m_childID( childID ), m_element( element ) {
			m_element["id"] = m_childID++;
		}
		void visit( const char* key, const char* value ) override {
			if ( string_equal_nocase( key, "_cbre_group" ) ) {
				return;
			}
			m_element[key] = value;
		}
	};
	class MapVMFWriteOutput : public Entity::OutputVisitor
	{
		kvpp::KV1ElementWritable<std::string>& m_element;
	public:
		MapVMFWriteOutput( kvpp::KV1ElementWritable<std::string>& element ) : m_element( element ) {
		}
		void visit( EntityOutput* output ) override {
			m_element["connections"].addChild( output->key(), output->value() );
		}
	};
	class MapVMFWalker : public scene::Traversable::Walker
	{
		mutable int64_t m_childID; // global element index, seems to be shared between entities and solids
		mutable std::size_t m_currentElement;
		mutable std::size_t m_worldElement;
		mutable std::vector<int64_t> m_groupStack;
		mutable std::vector<std::size_t> m_elementStack;
		kvpp::KV1Writer<std::string>& m_writer;

		int64_t currentGroup() const {
			return m_groupStack.empty() ? 0 : m_groupStack.back();
		}
		void writeEditor( kvpp::KV1ElementWritable<std::string>& element, int64_t groupid, const Layer* layer ) const {
			auto& editor = element.addChild("editor");
			editor["visgroupshown"] = 1;
			editor["visgroupautoshown"] = 1;
			if ( groupid > 0 ) {
				editor["groupid"] = groupid;
			}
			if ( layer != nullptr && layer->m_ownIndex > 0 ) {
				editor["visgroupid"] = layer->m_ownIndex;
			}
		}
	public:
		MapVMFWalker( kvpp::KV1Writer<std::string>& writer, std::size_t worldIndex ) : m_childID( 1 ), m_currentElement( worldIndex ), m_worldElement( worldIndex ), m_writer( writer ) {
		}
		class MapVMFTokenWriter : public TokenWriter
		{
			std::vector<std::string> m_lines;
		public:
			MapVMFTokenWriter() {
				m_lines.push_back("");
			}
			virtual void release() {
			}
			virtual void nextLine() {
				m_lines.push_back("");
			}
			virtual void writeToken( const char* token ) {
				m_lines.back().append( std::format( "{} ", token ) );
			}
			virtual void writeString( const char* string ) {
				m_lines.back().append( std::format( "\"{}\" ", string ) );
			}
			virtual void writeInteger( int i ) {
				m_lines.back().append( std::format( "{} ", i ) );
			}
			virtual void writeUnsigned( std::size_t i ) {
				m_lines.back().append( std::format( "{} ", i ) );
			}
			virtual void writeFloat( double f ) {
				m_lines.back().append( std::format( "{} ", f ) );
			}
			void forEachLine( int64_t& childID, kvpp::KV1ElementWritable<std::string>& solid, std::function<void(int64_t& childID, std::string& line, kvpp::KV1ElementWritable<std::string>& side)> func ) {
				for ( auto line : m_lines ) {
					if ( line[0] == '(' ) {
						func( childID, line, solid.addChild("side") );
					}
				}
			}
		};
		void writeSolid( scene::Node& node, kvpp::KV1ElementWritable<std::string>& solid, bool inWorld ) const {
			solid["id"] = m_childID++;

			MapExporter* exporter = Node_getMapExporter( node );

			// since VMFs are a different beast from MAP files, we need to use Radiant's normal exporter and then do our own thing with it
			// FIXME: this sucks and could surely be done better.
			MapVMFTokenWriter writer;
			exporter->exportTokens( writer );
			writer.forEachLine( m_childID, solid, [](int64_t& childID, std::string& line, kvpp::KV1ElementWritable<std::string>& side){
				double points[3][3];
				char material[128];
				double uaxis[4];
				double vaxis[4];
				double rotation;
				double scale[2];
				double lightmapscale;
				sscanf(line.c_str(), "( %lf %lf %lf ) ( %lf %lf %lf ) ( %lf %lf %lf ) %128s [ %lf %lf %lf %lf ] [ %lf %lf %lf %lf ] %lf %lf %lf %lf",
					&points[0][0], &points[0][1], &points[0][2], &points[1][0], &points[1][1], &points[1][2], &points[2][0], &points[2][1], &points[2][2],
					material, &uaxis[0], &uaxis[1], &uaxis[2], &uaxis[3], &vaxis[0], &vaxis[1], &vaxis[2], &vaxis[3], &rotation, &scale[0], &scale[1],
					&lightmapscale
				);
				side["id"] = childID++;
				side["plane"] = std::format( "({} {} {}) ({} {} {}) ({} {} {})", points[0][0], points[0][1], points[0][2], points[1][0], points[1][1], points[1][2], points[2][0], points[2][1], points[2][2] );
				side["material"] = material;
				side["uaxis"] = std::format( "[{} {} {} {}] {}", uaxis[0], uaxis[1], uaxis[2], uaxis[3], scale[0] );
				side["vaxis"] = std::format( "[{} {} {} {}] {}", vaxis[0], vaxis[1], vaxis[2], vaxis[3], scale[1] );
				side["rotation"] = std::format( "{}", rotation );
				side["lightmapscale"] = std::format( "{}", lightmapscale );
				side["smoothing_groups"] = 0; // FIXME: make configurable
			});
			writeEditor( solid, inWorld ? currentGroup() : 0, node.m_layer );
		}
		virtual bool pre( scene::Node& node ) const {
			Entity* entity = Node_getEntity( node );
			if ( entity ) {
				if ( string_equal( entity->getClassName(), "worldspawn" ) ) {
					MapVMFWriteKeyValue visitor( m_writer["world"], m_childID );
					entity->forEachKeyValue( visitor );
					MapVMFWriteOutput outputVisitor( m_writer["world"] );
					entity->forEachOutput( outputVisitor );
					const char *write_default_keyvalues = GlobalRadiant().getGameDescriptionKeyValue( "write_default_keyvalues" );
					if (write_default_keyvalues && string_equal(write_default_keyvalues, "1"))
					{
						const auto& entityClass = entity->getEntityClass();
						for (const auto& [ key, attr ] : entityClass.m_attributes)
						{
							if (!entity->hasKeyValue(key.c_str()))
							{
								m_writer["world"].addChild(key.c_str(), attr.m_value.c_str());
							}
						}
					}
					m_worldElement = m_writer.getChildCount() - 1;
					m_currentElement = m_worldElement;
					m_groupStack.clear();
					m_elementStack.clear();
				} else if ( entity_is_group( entity ) ) {
					const int64_t parentGroup = currentGroup();
					auto& group = m_writer[m_worldElement].addChild("group");
					const int64_t id = m_childID++;
					group["id"] = id;
					writeEditor( group, parentGroup, node.m_layer );
					m_groupStack.push_back( id );
				} else {
					auto& child = m_writer.addChild("entity");
					MapVMFWriteKeyValue visitor( child, m_childID );
					entity->forEachKeyValue( visitor );
					MapVMFWriteOutput outputVisitor( child );
					entity->forEachOutput( outputVisitor );
					const char *write_default_keyvalues = GlobalRadiant().getGameDescriptionKeyValue( "write_default_keyvalues" );
					if (write_default_keyvalues && string_equal(write_default_keyvalues, "1"))
					{
						const auto& entityClass = entity->getEntityClass();
						for (const auto& [ key, attr ] : entityClass.m_attributes)
						{
							if (!entity->hasKeyValue(key.c_str()))
							{
								child.addChild(key.c_str(), attr.m_value.c_str());
							}
						}
					}
					writeEditor( child, currentGroup(), node.m_layer );
					m_elementStack.push_back( m_currentElement );
					m_currentElement = m_writer.getChildCount() - 1;
				}
			} else if ( Node_isBrush( node ) ) {
				const bool inWorld = m_currentElement == m_worldElement;
				writeSolid( node, m_writer[m_currentElement].addChild("solid"), inWorld );
			}

			return true;
		}
		void post( scene::Node& node ) const override {
			Entity* entity = Node_getEntity( node );
			if ( entity == nullptr ) {
				return;
			}
			if ( string_equal( entity->getClassName(), "worldspawn" ) ) {
				return;
			}
			if ( entity_is_group( entity ) ) {
				if ( !m_groupStack.empty() ) {
					m_groupStack.pop_back();
				}
			} else if ( !m_elementStack.empty() ) {
				m_currentElement = m_elementStack.back();
				m_elementStack.pop_back();
			}
		}
	};
	void writeGraph( scene::Node& root, GraphTraversalFunc traverse, TextOutputStream& outputStream ) const override {

		// FIXME: should this be hardcoded?
		GlobalBrushCreator().toggleFormat(eBrushTypeValveSource);

		kvpp::KV1Writer writer;

		// make up some shit
		writer["versioninfo"]["editorextension"] = "wiferadiant"; // strata checks this
		writer["versioninfo"]["editorversion"] = RADIANT_VERSION;
		writer["versioninfo"]["editorbuild"] = RADIANT_GIT_REVISION;
		writer["versioninfo"]["mapversion"] = GlobalRadiant().Map_getVersion();
		writer["versioninfo"]["formatversion"] = 100; // not sure what this corresponds to
		writer["versioninfo"]["prefab"] = 0; // FIXME: make configurable

		// write visgroups
		if ( auto* layers = Node_getLayers( root ); layers != nullptr ) {
			layers->update_ownIndices();
			auto& visgroups = writer.addChild("visgroups");
			layers->forEach([&]( Layer& layer ){
				if ( layer.m_ownIndex <= 0 ) {
					return;
				}
				auto& visgroup = visgroups.addChild("visgroup");
				visgroup["name"] = layer.m_name.c_str();
				visgroup["visgroupid"] = layer.m_ownIndex;
				visgroup["color"] = std::format("{} {} {}", layer.m_color[0], layer.m_color[1], layer.m_color[2]);
			});
		}

		// traverse tree
		traverse(root, MapVMFWalker( writer, writer.getChildCount() ));

		// extra garbage
		writer["cameras"]["activecamera"] = -1; // shrug
		writer["cordon"]["active"] = 0;

		// bake and write
		auto baked = writer.bake();
		outputStream.write(baked.c_str(), baked.length());
	}
};

typedef SingletonModule<MapVMFAPI, MapDependencies> MapVMFModule;

MapVMFModule g_MapVMFModule;


extern "C" void RADIANT_DLLEXPORT Radiant_RegisterModules( ModuleServer& server ){
	initialiseModule( server );

	g_MapVMFModule.selfRegister();
}
