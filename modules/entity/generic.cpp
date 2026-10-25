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

///\file
///\brief Represents any entity which has a fixed size specified in its entity-definition and does not display a model (e.g. info_player_start).
///
/// This entity displays an axis-aligned bounding box of the size and colour specified in its entity-definition.
/// The "origin" key directly controls the entity's local-to-parent transform.
/// An arrow is drawn to visualise the "angle" key.

#include "cullable.h"
#include "renderable.h"
#include "editable.h"

#include "math/frustum.h"
#include "selectionlib.h"
#include "instancelib.h"
#include "transformlib.h"
#include "entitylib.h"
#include "render.h"
#include "eclasslib.h"
#include "math/line.h"

#include "targetable.h"
#include "origin.h"
#include "angles.h"
#include "filters.h"
#include "namedentity.h"
#include "keyobservers.h"
#include "namekeys.h"
#include "rotation.h"

#include "entity.h"

#include "ifilesystem.h"
#include "imagelib.h"
#include "qerplugin.h"
#include "stringio.h"
#include "texturelib.h"

#include <kvpp/kvpp.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>


inline void read_aabb( AABB& aabb, const EntityClass& eclass ){
	aabb = aabb_for_minmax( eclass.mins, eclass.maxs );
}

namespace
{
inline bool is_dys_screen_class( const char* name ){
	return string_equal( name, "dys_screen" ) || string_equal( name, "dys_cyberscreen" );
}

std::string read_archive_text( const char* path ){
	ArchiveFile* file = GlobalFileSystem().openFile( path );
	if ( file == nullptr ){
		return {};
	}

	ScopedArchiveBuffer buffer( *file );
	file->release();

	const char* data = reinterpret_cast<const char*>( buffer.buffer );
	const std::size_t length = buffer.length;
	if ( length >= 2 && static_cast<unsigned char>( data[0] ) == 0xFF && static_cast<unsigned char>( data[1] ) == 0xFE ){
		std::string text;
		text.reserve( length / 2 );
		for ( std::size_t i = 2; i + 1 < length; i += 2 ){
			const unsigned int c = static_cast<unsigned char>( data[i] ) | ( static_cast<unsigned int>( static_cast<unsigned char>( data[i + 1] ) ) << 8 );
			text.push_back( c < 0x80 ? static_cast<char>( c ) : '?' );
		}
		return text;
	}
	if ( length >= 2 && static_cast<unsigned char>( data[0] ) == 0xFE && static_cast<unsigned char>( data[1] ) == 0xFF ){
		std::string text;
		text.reserve( length / 2 );
		for ( std::size_t i = 2; i + 1 < length; i += 2 ){
			const unsigned int c = ( static_cast<unsigned int>( static_cast<unsigned char>( data[i] ) ) << 8 ) | static_cast<unsigned char>( data[i + 1] );
			text.push_back( c < 0x80 ? static_cast<char>( c ) : '?' );
		}
		return text;
	}
	if ( length >= 3 && static_cast<unsigned char>( data[0] ) == 0xEF && static_cast<unsigned char>( data[1] ) == 0xBB && static_cast<unsigned char>( data[2] ) == 0xBF ){
		return std::string( data + 3, length - 3 );
	}
	return std::string( data, length );
}

struct ScreenControl
{
	std::string type;
	std::string material;
	std::string enabledMaterial;
	std::string labelText;
	int xpos = 0;
	int ypos = 0;
	int zpos = 0;
	int wide = 0;
	int tall = 0;
	int textAlignment = 0;
	bool visible = true;
	bool background = false;
};

std::vector<ScreenControl> parse_screen_controls( const char* panelname, std::string& backgroundMaterial ){
	backgroundMaterial.clear();
	std::vector<ScreenControl> controls;
	if ( string_empty( panelname ) ){
		return controls;
	}

	char path[260];
	std::snprintf( path, sizeof( path ), "scripts/screens/%s.res", panelname );
	const std::string text = read_archive_text( path );
	if ( text.empty() ){
		return controls;
	}

	kvpp::KV1 kv1( text );
	if ( kv1.getChildCount() == 0 ){
		return controls;
	}

	for ( const auto& element : kv1[0] ){
		ScreenControl control;
		control.type = std::string( element["ControlName"].getValue() );
		control.material = std::string( element["material"].getValue() );
		control.enabledMaterial = std::string( element["enabledImage"]["material"].getValue() );
		control.labelText = std::string( element["labelText"].getValue() );
		control.xpos = element["xpos"].getValue<int>();
		control.ypos = element["ypos"].getValue<int>();
		control.zpos = element["zpos"].getValue<int>();
		control.wide = element["wide"].getValue<int>();
		control.tall = element["tall"].getValue<int>();

		const std::string_view alignment = element["textAlignment"].getValue();
		if ( alignment == "center" ){
			control.textAlignment = 1;
		}
		else if ( alignment == "right" ){
			control.textAlignment = 2;
		}
		else{
			control.textAlignment = element["textAlignment"].getValue<int>();
		}

		const auto& visible = element["visible"];
		control.visible = visible.isInvalid() || visible.getValue<int>() != 0;

		controls.push_back( std::move( control ) );
	}

	int bestZPos = INT_MAX;
	int backgroundIndex = -1;
	for ( std::size_t i = 0; i < controls.size(); ++i ){
		if ( controls[i].type == "MaterialImage" && !controls[i].material.empty() ){
			if ( backgroundIndex < 0 || controls[i].zpos < bestZPos ){
				bestZPos = controls[i].zpos;
				backgroundIndex = static_cast<int>( i );
			}
		}
	}
	if ( backgroundIndex >= 0 ){
		controls[backgroundIndex].background = true;
		backgroundMaterial = controls[backgroundIndex].material;
	}

	return controls;
}

std::string resolve_localized_text( const std::string& text ){
	if ( text.empty() || text[0] != '#' ){
		return text;
	}

	static std::map<std::string, std::string> tokens;
	static bool loaded = false;
	if ( !loaded ){
		loaded = true;
		const std::string data = read_archive_text( "resource/dystopia_english.txt" );
		if ( !data.empty() ){
			kvpp::KV1 kv1( data );
			if ( kv1.getChildCount() != 0 ){
				for ( const auto& token : kv1[0]["Tokens"] ){
					tokens[std::string( token.getKey() )] = std::string( token.getValue() );
				}
			}
		}
	}

	const std::string key = text.substr( 1 );
	const auto it = tokens.find( key );
	return it != tokens.end() ? it->second : key;
}

std::string game_texture_prefix(){
	const char* prefix = GlobalRadiant().getGameDescriptionKeyValue( "shaderpath" );
	if ( string_empty( prefix ) ){
		prefix = GlobalRadiant().getGameDescriptionKeyValue( "texturepath" );
	}

	std::string result = prefix;
	if ( !result.empty() && result.back() != '/' && result.back() != '\\' ){
		result += '/';
	}
	return result;
}

std::string editor_screen_material( const std::string& material ){
	if ( material.empty() ){
		return material;
	}

	const std::string prefix = game_texture_prefix();
	const std::string path = prefix + material + ".vmt";
	const std::string text = read_archive_text( path.c_str() );
	if ( text.empty() ){
		return material;
	}

	kvpp::KV1 kv1( text );
	if ( kv1.getChildCount() == 0 ){
		return material;
	}

	const std::string baseTexture = std::string( kv1[0]["$basetexture"].getValue() );
	const std::string toolTexture = std::string( kv1[0]["%tooltexture"].getValue() );
	if ( !toolTexture.empty() && ( baseTexture.empty() || baseTexture.rfind( "_rt_", 0 ) == 0 ) ){
		return toolTexture;
	}
	return material;
}

std::string material_shader_name( const std::string& material ){
	if ( material.empty() ){
		return {};
	}

	std::string name = material;
	for ( char& c : name ){
		if ( c == '\\' ){
			c = '/';
		}
		else{
			c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
		}
	}

	const std::string prefix = game_texture_prefix();
	if ( !string_equal_prefix_nocase( name.c_str(), prefix.c_str() ) ){
		name = prefix + name;
	}
	return name;
}

Matrix4 screen_rotation( const Vector3& angles ){
	Matrix4 rotation = matrix4_rotation_for_euler_xyz_degrees( angles );
	matrix4_multiply_by_matrix4( rotation, matrix4_rotation_for_y_degrees( 90.0 ) );
	matrix4_multiply_by_matrix4( rotation, matrix4_rotation_for_z_degrees( 90.0 ) );
	return rotation;
}

struct ScreenLayer
{
	float x0 = 0;
	float y0 = 0;
	float x1 = 0;
	float y1 = 0;
	float u0 = 0;
	float v0 = 0;
	float u1 = 1;
	float v1 = 1;
	std::string shaderName;
	Shader* shader = nullptr;
	GLuint texture = 0;
};
}


// draws the iconsprite() of the class
class RenderableIcon : public OpenGLRenderable
{
public:
	mutable bool m_selected = false;

	void render( RenderStateFlags state ) const override {
		const float size = 16.f;

		GLfloat modelview[16];
		gl().glGetFloatv( GL_MODELVIEW_MATRIX, modelview );
		const float sx = std::sqrt( modelview[0] * modelview[0] + modelview[1] * modelview[1] + modelview[2] * modelview[2] );
		const float sy = std::sqrt( modelview[4] * modelview[4] + modelview[5] * modelview[5] + modelview[6] * modelview[6] );
		const float sz = std::sqrt( modelview[8] * modelview[8] + modelview[9] * modelview[9] + modelview[10] * modelview[10] );
		gl().glPushMatrix();
		modelview[0] = sx; modelview[1] = 0; modelview[2] = 0;
		modelview[4] = 0; modelview[5] = sy; modelview[6] = 0;
		modelview[8] = 0; modelview[9] = 0; modelview[10] = sz;
		gl().glLoadMatrixf( modelview );

		gl().glPushAttrib( GL_ALL_ATTRIB_BITS );
		gl().glDisable( GL_CULL_FACE );
		gl().glDisable( GL_LIGHTING );
		gl().glEnable( GL_TEXTURE_2D );
		gl().glEnable( GL_ALPHA_TEST );
		gl().glAlphaFunc( GL_GREATER, 0.5f );
		gl().glEnable( GL_BLEND );
		gl().glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
		gl().glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		gl().glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		if ( m_selected ) {
			gl().glColor3f( 1.0f, 0.63f, 0.0f );
		}
		else {
			gl().glColor3f( 1.0f, 1.0f, 1.0f );
		}

		gl().glBegin( GL_QUADS );
		gl().glTexCoord2f( 0, 0 );
		gl().glVertex3f( -size, size, 0 );
		gl().glTexCoord2f( 1, 0 );
		gl().glVertex3f( size, size, 0 );
		gl().glTexCoord2f( 1, 1 );
		gl().glVertex3f( size, -size, 0 );
		gl().glTexCoord2f( 0, 1 );
		gl().glVertex3f( -size, -size, 0 );
		gl().glEnd();

		gl().glPopAttrib();

		gl().glPopMatrix();
	}
};

class RenderableScreen : public OpenGLRenderable
{
public:
	mutable bool m_selected = false;
	mutable bool m_hasTexture = false;
	mutable Matrix4 m_rotation = g_matrix4_identity;
	const std::vector<ScreenLayer>* m_layers = nullptr;
	GLuint m_baseTexture = 0;
	float m_width = 32.f;
	float m_height = 32.f;

	void render( RenderStateFlags ) const override {
		gl().glPushAttrib( GL_ALL_ATTRIB_BITS );
		gl().glDisable( GL_CULL_FACE );
		gl().glDisable( GL_LIGHTING );
		if ( m_hasTexture ){
			gl().glEnable( GL_TEXTURE_2D );
		}
		else{
			gl().glDisable( GL_TEXTURE_2D );
		}
		gl().glEnable( GL_BLEND );
		gl().glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

		if ( m_selected ){
			gl().glColor4f( 1.0f, 0.63f, 0.0f, 1.0f );
		}
		else{
			gl().glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
		}

		gl().glMatrixMode( GL_MODELVIEW );
		gl().glPushMatrix();
		gl().glMultMatrixf( reinterpret_cast<const float*>( &m_rotation ) );

		gl().glBegin( GL_QUADS );
		gl().glTexCoord2f( 0, 1 );
		gl().glVertex3f( 0, 0, 0 );
		gl().glTexCoord2f( 1, 1 );
		gl().glVertex3f( m_width, 0, 0 );
		gl().glTexCoord2f( 1, 0 );
		gl().glVertex3f( m_width, m_height, 0 );
		gl().glTexCoord2f( 0, 0 );
		gl().glVertex3f( 0, m_height, 0 );
		gl().glEnd();

		if ( m_hasTexture && m_layers != nullptr && !m_layers->empty() ){
			gl().glDepthMask( GL_FALSE );
			gl().glDepthFunc( GL_LEQUAL );
			gl().glEnable( GL_ALPHA_TEST );
			gl().glAlphaFunc( GL_GREATER, 0.01f );
			gl().glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

			const float z = 0.05f;
			for ( const ScreenLayer& layer : *m_layers ){
				const GLuint texture = layer.shader != nullptr ? layer.shader->getTexture().texture_number : layer.texture;
				if ( texture == 0 ){
					continue;
				}

				gl().glBindTexture( GL_TEXTURE_2D, texture );
				gl().glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
				gl().glBegin( GL_QUADS );
				gl().glTexCoord2f( layer.u0, layer.v1 );
				gl().glVertex3f( layer.x0, layer.y0, z );
				gl().glTexCoord2f( layer.u1, layer.v1 );
				gl().glVertex3f( layer.x1, layer.y0, z );
				gl().glTexCoord2f( layer.u1, layer.v0 );
				gl().glVertex3f( layer.x1, layer.y1, z );
				gl().glTexCoord2f( layer.u0, layer.v0 );
				gl().glVertex3f( layer.x0, layer.y1, z );
				gl().glEnd();
			}

			if ( m_baseTexture != 0 ){
				gl().glBindTexture( GL_TEXTURE_2D, m_baseTexture );
			}
			gl().glDepthMask( GL_TRUE );
		}

		gl().glPopMatrix();
		gl().glPopAttrib();
	}
};

class RenderableScreenOutline : public OpenGLRenderable
{
public:
	mutable bool m_selected = false;
	mutable Matrix4 m_rotation = g_matrix4_identity;
	float m_width = 32.f;
	float m_height = 32.f;

	void render( RenderStateFlags ) const override {
		gl().glPushAttrib( GL_ALL_ATTRIB_BITS );
		gl().glDisable( GL_CULL_FACE );
		gl().glDisable( GL_LIGHTING );
		gl().glDisable( GL_TEXTURE_2D );
		gl().glEnable( GL_BLEND );
		gl().glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		gl().glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		gl().glLineWidth( 1.5f );

		if ( m_selected ){
			gl().glColor4f( 1.0f, 0.63f, 0.0f, 1.0f );
		}
		else{
			gl().glColor4f( 0.65f, 0.85f, 1.0f, 1.0f );
		}

		gl().glMatrixMode( GL_MODELVIEW );
		gl().glPushMatrix();
		gl().glMultMatrixf( reinterpret_cast<const float*>( &m_rotation ) );

		gl().glBegin( GL_LINE_LOOP );
		gl().glVertex3f( 0, 0, 0 );
		gl().glVertex3f( m_width, 0, 0 );
		gl().glVertex3f( m_width, m_height, 0 );
		gl().glVertex3f( 0, m_height, 0 );
		gl().glEnd();

		gl().glBegin( GL_LINES );
		gl().glVertex3f( 0, 0, 0 );
		gl().glVertex3f( 0, 0, std::min( m_width, m_height ) * 0.35f );
		gl().glEnd();

		gl().glPopMatrix();
		gl().glPopAttrib();
	}
};

class GenericEntity :
	public Cullable,
	public Bounded,
	public Snappable
{
	EntityKeyValues m_entity;
	KeyObserverMap m_keyObservers;
	MatrixTransform m_transform;

	OriginKey m_originKey;
	Vector3 m_origin;
	AnglesKey m_anglesKey;
	Vector3 m_angles;

	ClassnameFilter m_filter;
	NamedEntity m_named;
	NameKeys m_nameKeys;

	AABB m_aabb_local;

	RenderableArrow m_arrow;
	RenderableSolidAABB m_aabb_solid;
	RenderableWireframeAABB m_aabb_wire;
	RenderableIcon m_icon;
	RenderableNamedEntity m_renderName;

	bool m_screenEntity = false;
	bool m_screenAttached = false;
	float m_screenWidth = 32.f;
	float m_screenHeight = 32.f;
	int m_screenResWidth = 256;
	int m_screenResHeight = 256;
	std::string m_screenMaterial;
	std::string m_screenShaderName;
	Shader* m_screenShader = nullptr;
	std::vector<ScreenControl> m_screenControls;
	std::vector<ScreenLayer> m_screenLayers;
	RenderableScreen m_screen;
	RenderableScreenOutline m_screenOutline;

	Callback<void()> m_transformChanged;
	Callback<void()> m_evaluateTransform;

	void construct(){
		m_screenEntity = is_dys_screen_class( m_entity.getEntityClass().name() );
		read_aabb( m_aabb_local, m_entity.getEntityClass() );

		m_keyObservers.insert( "classname", ClassnameFilter::ClassnameChangedCaller( m_filter ) );
		m_keyObservers.insert( Static<KeyIsName>::instance().m_nameKey, NamedEntity::IdentifierChangedCaller( m_named ) );
		if( m_entity.getEntityClass().has_direction_key )
			m_keyObservers.insert( "angle", m_anglesKey.getGroupAngleChangedCallback() );
		else
			m_keyObservers.insert( "angle", m_anglesKey.getAngleChangedCallback() );
		m_keyObservers.insert( "angles", m_anglesKey.getAnglesChangedCallback() );
		m_keyObservers.insert( "origin", OriginKey::OriginChangedCaller( m_originKey ) );

		if ( m_screenEntity ) {
			m_screen.m_layers = &m_screenLayers;
			m_keyObservers.insert( "width", ScreenChangedCaller( *this ) );
			m_keyObservers.insert( "height", ScreenChangedCaller( *this ) );
			m_keyObservers.insert( "reswidth", ScreenChangedCaller( *this ) );
			m_keyObservers.insert( "resheight", ScreenChangedCaller( *this ) );
			m_keyObservers.insert( "panelname", ScreenChangedCaller( *this ) );
			screenChanged();
		}
	}

// vc 2k5 compiler fix
#if _MSC_VER >= 1400
public:
#endif

	void updateTransform(){
		m_transform.localToParent() = g_matrix4_identity;
		matrix4_translate_by_vec3( m_transform.localToParent(), m_origin );
		m_transformChanged();
	}
	typedef MemberCaller<GenericEntity, void(), &GenericEntity::updateTransform> UpdateTransformCaller;
	void originChanged(){
		m_origin = m_originKey.m_origin;
		updateTransform();
	}
	typedef MemberCaller<GenericEntity, void(), &GenericEntity::originChanged> OriginChangedCaller;
	void anglesChanged(){
		m_angles = m_anglesKey.m_angles;
		if ( m_screenEntity ) {
			updateScreenAABB();
		}
		updateTransform();
	}
	typedef MemberCaller<GenericEntity, void(), &GenericEntity::anglesChanged> AnglesChangedCaller;

	const char* screenKeyValue( const char* key ) const {
		const char* value = m_entity.getKeyValue( key );
		if ( string_empty( value ) ) {
			value = EntityClass_valueForKey( m_entity.getEntityClass(), key );
		}
		return value;
	}
	void updateScreenAABB(){
		const Vector3 corners[4] = {
			Vector3( 0, 0, 0 ),
			Vector3( m_screenWidth, 0, 0 ),
			Vector3( m_screenWidth, m_screenHeight, 0 ),
			Vector3( 0, m_screenHeight, 0 ),
		};

		const Matrix4 rotation = screen_rotation( m_angles );
		Vector3 mins = matrix4_transformed_point( rotation, corners[0] );
		Vector3 maxs = mins;
		for ( int i = 1; i < 4; ++i ) {
			const Vector3 point = matrix4_transformed_point( rotation, corners[i] );
			for ( int axis = 0; axis < 3; ++axis ) {
				mins[axis] = std::min( mins[axis], point[axis] );
				maxs[axis] = std::max( maxs[axis], point[axis] );
			}
		}

		const Vector3 padding( 1.f, 1.f, 1.f );
		m_aabb_local = aabb_for_minmax( vector3_subtracted( mins, padding ), vector3_added( maxs, padding ) );
	}
	void releaseScreenShader(){
		if ( m_screenShader != nullptr ) {
			GlobalShaderCache().release( m_screenShaderName.c_str() );
			m_screenShader = nullptr;
		}
		m_screen.m_hasTexture = false;
		m_screen.m_baseTexture = 0;
	}
	void releaseScreenLayers(){
		for ( ScreenLayer& layer : m_screenLayers ){
			if ( layer.shader != nullptr ){
				GlobalShaderCache().release( layer.shaderName.c_str() );
				layer.shader = nullptr;
			}
			if ( layer.texture != 0 ){
				gl().glDeleteTextures( 1, &layer.texture );
				layer.texture = 0;
			}
		}
		m_screenLayers.clear();
	}
	void screenShaderChanged(){
		const std::string shaderName = material_shader_name( m_screenMaterial );
		if ( shaderName == m_screenShaderName && ( m_screenShader != nullptr || !m_screenAttached ) ) {
			m_screen.m_hasTexture = ( m_screenShader != nullptr );
			m_screen.m_baseTexture = ( m_screenShader != nullptr ) ? m_screenShader->getTexture().texture_number : 0;
			return;
		}

		releaseScreenShader();
		m_screenShaderName = shaderName;
		if ( m_screenAttached && !shaderName.empty() ) {
			m_screenShader = GlobalShaderCache().capture( shaderName.c_str() );
		}
		m_screen.m_hasTexture = ( m_screenShader != nullptr );
		m_screen.m_baseTexture = ( m_screenShader != nullptr ) ? m_screenShader->getTexture().texture_number : 0;
	}
	void addScreenImageLayer( float u0, float v0, float u1, float v1, const std::string& material ){
		const std::string shaderName = material_shader_name( editor_screen_material( material ) );
		if ( shaderName.empty() ) {
			return;
		}

		Shader* shader = GlobalShaderCache().capture( shaderName.c_str() );
		if ( shader == nullptr ) {
			return;
		}

		ScreenLayer layer;
		layer.shaderName = shaderName;
		layer.shader = shader;
		layer.x0 = u0 * m_screenWidth;
		layer.x1 = u1 * m_screenWidth;
		layer.y0 = m_screenHeight * ( 1.f - v1 );
		layer.y1 = m_screenHeight * ( 1.f - v0 );
		m_screenLayers.push_back( layer );
	}
	void addScreenTextLayer( const ScreenControl& control, const std::string& text, int resWidth, int resHeight ){
		if ( GlobalOpenGL().m_font == nullptr ) {
			return;
		}

		GLuint texture = 0;
		gl().glGenTextures( 1, &texture );
		if ( texture == 0 ) {
			return;
		}

		unsigned int textWidth = 0;
		unsigned int textHeight = 0;
		const unsigned char colour[3] = { 200, 200, 200 };
		GlobalOpenGL().m_font->renderString( text.c_str(), texture, colour, textWidth, textHeight );
		if ( textWidth == 0 || textHeight == 0 ) {
			gl().glDeleteTextures( 1, &texture );
			return;
		}

		const int controlWidth = std::max( 1, control.wide );
		const int controlHeight = std::max( 1, control.tall );
		int drawHeight = std::min( controlHeight, std::max( 8, ( resHeight * 16 ) / 256 ) );
		int drawWidth = static_cast<int>( drawHeight * static_cast<float>( textWidth ) / static_cast<float>( textHeight ) );
		if ( drawWidth > controlWidth ) {
			drawWidth = controlWidth;
			drawHeight = static_cast<int>( drawWidth * static_cast<float>( textHeight ) / static_cast<float>( textWidth ) );
		}

		int drawX = control.xpos;
		if ( control.textAlignment == 1 ) {
			drawX += ( controlWidth - drawWidth ) / 2;
		}
		else if ( control.textAlignment == 2 ) {
			drawX += controlWidth - drawWidth;
		}
		const int drawY = control.ypos + ( controlHeight - drawHeight ) / 2;

		ScreenLayer layer;
		layer.texture = texture;
		layer.x0 = ( static_cast<float>( drawX ) / resWidth ) * m_screenWidth;
		layer.x1 = ( static_cast<float>( drawX + drawWidth ) / resWidth ) * m_screenWidth;
		layer.y0 = m_screenHeight * ( 1.f - static_cast<float>( drawY + drawHeight ) / resHeight );
		layer.y1 = m_screenHeight * ( 1.f - static_cast<float>( drawY ) / resHeight );
		layer.u0 = 0.f;
		layer.v0 = 0.f;
		layer.u1 = 1.f / 3.f;
		layer.v1 = 1.f;
		m_screenLayers.push_back( layer );
	}
	void rebuildScreenLayers(){
		releaseScreenLayers();
		if ( !m_screenAttached || m_screenShader == nullptr ) {
			return;
		}

		const int resWidth = m_screenResWidth > 0 ? m_screenResWidth : 256;
		const int resHeight = m_screenResHeight > 0 ? m_screenResHeight : 256;

		std::vector<const ScreenControl*> sorted;
		for ( const ScreenControl& control : m_screenControls ){
			if ( control.visible && !control.background ) {
				sorted.push_back( &control );
			}
		}
		std::stable_sort( sorted.begin(), sorted.end(), []( const ScreenControl* a, const ScreenControl* b ){
			return a->zpos < b->zpos;
		} );

		for ( const ScreenControl* control : sorted ){
			float u0 = static_cast<float>( control->xpos ) / resWidth;
			float v0 = static_cast<float>( control->ypos ) / resHeight;
			float u1 = static_cast<float>( control->xpos + control->wide ) / resWidth;
			float v1 = static_cast<float>( control->ypos + control->tall ) / resHeight;
			u0 = std::min( 1.f, std::max( 0.f, u0 ) );
			v0 = std::min( 1.f, std::max( 0.f, v0 ) );
			u1 = std::min( 1.f, std::max( 0.f, u1 ) );
			v1 = std::min( 1.f, std::max( 0.f, v1 ) );
			if ( u1 <= u0 || v1 <= v0 ) {
				continue;
			}

			const std::string& material = !control->enabledMaterial.empty() ? control->enabledMaterial : control->material;
			if ( !material.empty() ) {
				addScreenImageLayer( u0, v0, u1, v1, material );
			}

			if ( ( control->type == "Label" || control->type == "MaterialButton" ) && !control->labelText.empty() ) {
				const std::string text = resolve_localized_text( control->labelText );
				if ( !text.empty() ) {
					addScreenTextLayer( *control, text, resWidth, resHeight );
				}
			}
		}
	}
	void screenChanged(){
		const float width = string_read_float( screenKeyValue( "width" ) );
		const float height = string_read_float( screenKeyValue( "height" ) );
		const int resWidth = static_cast<int>( string_read_float( screenKeyValue( "reswidth" ) ) );
		const int resHeight = static_cast<int>( string_read_float( screenKeyValue( "resheight" ) ) );
		m_screenWidth = width > 0.f ? width : 32.f;
		m_screenHeight = height > 0.f ? height : 32.f;
		m_screenResWidth = resWidth > 0 ? resWidth : 256;
		m_screenResHeight = resHeight > 0 ? resHeight : 256;

		m_screen.m_width = m_screenWidth;
		m_screen.m_height = m_screenHeight;
		m_screenOutline.m_width = m_screenWidth;
		m_screenOutline.m_height = m_screenHeight;

		std::string backgroundMaterial;
		m_screenControls = parse_screen_controls( screenKeyValue( "panelname" ), backgroundMaterial );
		m_screenMaterial = editor_screen_material( backgroundMaterial );
		if ( m_screenMaterial.empty() ) {
			m_screenMaterial = "vgui/screens/vgui_bg";
		}

		screenShaderChanged();
		rebuildScreenLayers();
		updateScreenAABB();
	}
	void screenKeyChanged( const char* ) {
		screenChanged();
	}
	typedef MemberCaller<GenericEntity, void(const char*), &GenericEntity::screenKeyChanged> ScreenChangedCaller;
public:

	GenericEntity( EntityClass* eclass, scene::Node& node, const Callback<void()>& transformChanged, const Callback<void()>& evaluateTransform ) :
		m_entity( eclass ),
		m_originKey( OriginChangedCaller( *this ) ),
		m_origin( ORIGINKEY_IDENTITY ),
		m_anglesKey( AnglesChangedCaller( *this ), m_entity ),
		m_angles( ANGLESKEY_IDENTITY ),
		m_filter( m_entity, node ),
		m_named( m_entity ),
		m_nameKeys( m_entity ),
		m_arrow( m_aabb_local.origin, m_angles ),
		m_aabb_solid( m_aabb_local ),
		m_aabb_wire( m_aabb_local ),
		m_renderName( m_named, g_vector3_identity ),
		m_transformChanged( transformChanged ),
		m_evaluateTransform( evaluateTransform ){
		construct();
	}
	GenericEntity( const GenericEntity& other, scene::Node& node, const Callback<void()>& transformChanged, const Callback<void()>& evaluateTransform ) :
		m_entity( other.m_entity ),
		m_originKey( OriginChangedCaller( *this ) ),
		m_origin( ORIGINKEY_IDENTITY ),
		m_anglesKey( AnglesChangedCaller( *this ), m_entity ),
		m_angles( ANGLESKEY_IDENTITY ),
		m_filter( m_entity, node ),
		m_named( m_entity ),
		m_nameKeys( m_entity ),
		m_arrow( m_aabb_local.origin, m_angles ),
		m_aabb_solid( m_aabb_local ),
		m_aabb_wire( m_aabb_local ),
		m_renderName( m_named, g_vector3_identity ),
		m_transformChanged( transformChanged ),
		m_evaluateTransform( evaluateTransform ){
		construct();
	}
	~GenericEntity(){
		releaseScreenShader();
		releaseScreenLayers();
	}

	InstanceCounter m_instanceCounter;
	void instanceAttach( const scene::Path& path ){
		if ( ++m_instanceCounter.m_count == 1 ) {
			m_filter.instanceAttach();
			m_entity.instanceAttach( path_find_mapfile( path.begin(), path.end() ) );
			m_entity.attach( m_keyObservers );
			if ( m_screenEntity ) {
				m_screenAttached = true;
				screenShaderChanged();
				rebuildScreenLayers();
			}
		}
	}
	void instanceDetach( const scene::Path& path ){
		if ( --m_instanceCounter.m_count == 0 ) {
			if ( m_screenEntity ) {
				m_screenAttached = false;
				releaseScreenShader();
				releaseScreenLayers();
			}
			m_entity.detach( m_keyObservers );
			m_entity.instanceDetach( path_find_mapfile( path.begin(), path.end() ) );
			m_filter.instanceDetach();
		}
	}

	EntityKeyValues& getEntity(){
		return m_entity;
	}
	const EntityKeyValues& getEntity() const {
		return m_entity;
	}

	Namespaced& getNamespaced(){
		return m_nameKeys;
	}
	Nameable& getNameable(){
		return m_named;
	}
	TransformNode& getTransformNode(){
		return m_transform;
	}

	const AABB& localAABB() const override {
		return m_aabb_local;
	}

	VolumeIntersectionValue intersectVolume( const VolumeTest& volume, const Matrix4& localToWorld ) const override {
		return volume.TestAABB( localAABB(), localToWorld );
	}

	void renderArrow( Renderer& renderer, const VolumeTest& volume, const Matrix4& localToWorld ) const {
		if ( g_showAngles ) {
			renderer.addRenderable( m_arrow, localToWorld );
		}
	}
	void renderIcon( Renderer& renderer, const Matrix4& localToWorld, Renderer::EStyle style, bool selected ) const {
		Shader* icon = m_entity.getEntityClass().m_state_icon;
		if ( icon != nullptr ) {
			m_icon.m_selected = selected;
			// we draw the icon outside of the highlight pass so selecting it will tint the image instead of just overlaying a REALLY FUCKING BRIGHT quad
			renderer.PushState();
			renderer.Highlight( Renderer::ePrimitive, false );
			renderer.Highlight( Renderer::eFace, false );
			renderer.SetState( icon, style );
			renderer.addRenderable( m_icon, localToWorld );
			renderer.PopState();
		}
	}
	void renderScreen( Renderer& renderer, const Matrix4& localToWorld, bool selected, bool solid ) const {
		m_screen.m_selected = selected;
		m_screenOutline.m_selected = selected;

		const Matrix4 rotation = screen_rotation( m_angles );
		m_screen.m_rotation = rotation;
		m_screenOutline.m_rotation = rotation;

		if ( solid ) {
			Shader* shader = m_screenShader != nullptr
				? m_screenShader
				: m_entity.getEntityClass().m_state_fill;
			if ( shader != nullptr ) {
				renderer.PushState();
				renderer.Highlight( Renderer::ePrimitive, false );
				renderer.Highlight( Renderer::eFace, false );
				renderer.SetState( shader, Renderer::eFullMaterials );
				renderer.addRenderable( m_screen, localToWorld );
				renderer.PopState();
			}
		}

		renderer.PushState();
		renderer.Highlight( Renderer::ePrimitive, false );
		renderer.SetState( m_entity.getEntityClass().m_state_wire, solid ? Renderer::eFullMaterials : Renderer::eWireframeOnly );
		renderer.addRenderable( m_screenOutline, localToWorld );
		renderer.PopState();
	}
	void renderSolid( Renderer& renderer, const VolumeTest& volume, const Matrix4& localToWorld, bool selected ) const {
		if ( m_screenEntity ) {
			renderer.SetState( m_entity.getEntityClass().m_state_fill, Renderer::eFullMaterials );
			renderScreen( renderer, localToWorld, selected, true );
			renderArrow( renderer, volume, localToWorld );
			if ( g_showNames || selected ) {
				m_renderName.render( renderer, volume, localToWorld, selected );
			}
			return;
		}

		const bool hasIcon = m_entity.getEntityClass().m_state_icon != nullptr;
		renderer.SetState( m_entity.getEntityClass().m_state_fill, Renderer::eFullMaterials );
		if ( !hasIcon ) {
			renderer.addRenderable( m_aabb_solid, localToWorld );
		}
		renderIcon( renderer, localToWorld, Renderer::eFullMaterials, selected );
		renderArrow( renderer, volume, localToWorld );
		if ( g_showNames || selected ) {
			m_renderName.render( renderer, volume, localToWorld, selected );
		}
	}
	void renderWireframe( Renderer& renderer, const VolumeTest& volume, const Matrix4& localToWorld, bool selected ) const {
		if ( m_screenEntity ) {
			renderer.SetState( m_entity.getEntityClass().m_state_wire, Renderer::eWireframeOnly );
			renderer.addRenderable( m_aabb_wire, localToWorld );
			renderScreen( renderer, localToWorld, selected, false );
			renderArrow( renderer, volume, localToWorld );
			if ( selected || ( g_showNames && aabb_fits_view( m_aabb_local, volume.GetModelview(), volume.GetViewport(), g_showNamesRatio ) ) ) {
				m_renderName.render( renderer, volume, localToWorld, selected );
			}
			return;
		}

		const bool hasIcon = m_entity.getEntityClass().m_state_icon != nullptr;
		renderer.SetState( m_entity.getEntityClass().m_state_wire, Renderer::eWireframeOnly );
		if ( !hasIcon ) {
			renderer.addRenderable( m_aabb_wire, localToWorld );
		}
		renderIcon( renderer, localToWorld, Renderer::eWireframeOnly, selected );
		renderArrow( renderer, volume, localToWorld );
		if ( selected || ( g_showNames && aabb_fits_view( m_aabb_local, volume.GetModelview(), volume.GetViewport(), g_showNamesRatio ) ) ) {
			m_renderName.render( renderer, volume, localToWorld, selected );
		}
	}


	void testSelect( Selector& selector, SelectionTest& test, const Matrix4& localToWorld ){
		test.BeginMesh( localToWorld );

		SelectionIntersection best;
		aabb_testselect( m_aabb_local, test, best );
		if ( best.valid() ) {
			selector.addIntersection( best );
		}
	}

	void translate( const Vector3& translation ){
		m_origin = origin_translated( m_origin, translation );
	}
	void rotate( const Quaternion& rotation ){
		m_angles = angles_rotated( m_angles, rotation );
	}
	void snapto( float snap ) override {
		m_originKey.m_origin = origin_snapped( m_originKey.m_origin, snap );
		m_originKey.write( &m_entity );
	}
	void revertTransform(){
		m_origin = m_originKey.m_origin;
		m_angles = m_anglesKey.m_angles;
	}
	void freezeTransform(){
		m_originKey.m_origin = m_origin;
		m_originKey.write( &m_entity );
		if( m_anglesKey.m_angles != m_angles ){
			m_anglesKey.m_angles = m_angles;
			m_anglesKey.write( &m_entity );
		}
	}
	void transformChanged(){
		revertTransform();
		m_evaluateTransform();
		updateTransform();
	}
	typedef MemberCaller<GenericEntity, void(), &GenericEntity::transformChanged> TransformChangedCaller;
};

class GenericEntityInstance :
	public TargetableInstance,
	public TransformModifier,
	public Renderable,
	public SelectionTestable
{
	class TypeCasts
	{
		InstanceTypeCastTable m_casts;
	public:
		TypeCasts(){
			m_casts = TargetableInstance::StaticTypeCasts::instance().get();
			InstanceContainedCast<GenericEntityInstance, Bounded>::install( m_casts );
			InstanceContainedCast<GenericEntityInstance, Cullable>::install( m_casts );
			InstanceStaticCast<GenericEntityInstance, Renderable>::install( m_casts );
			InstanceStaticCast<GenericEntityInstance, SelectionTestable>::install( m_casts );
			InstanceStaticCast<GenericEntityInstance, Transformable>::install( m_casts );
			InstanceIdentityCast<GenericEntityInstance>::install( m_casts );
		}
		InstanceTypeCastTable& get(){
			return m_casts;
		}
	};

	GenericEntity& m_contained;
	mutable AABB m_bounds;
public:

	typedef LazyStatic<TypeCasts> StaticTypeCasts;

	Bounded& get( NullType<Bounded> ){
		return m_contained;
	}
	Cullable& get( NullType<Cullable> ){
		return m_contained;
	}

	STRING_CONSTANT( Name, "GenericEntityInstance" );

	GenericEntityInstance( const scene::Path& path, scene::Instance* parent, GenericEntity& contained ) :
		TargetableInstance( path, parent, this, StaticTypeCasts::instance().get(), contained.getEntity(), *this ),
		TransformModifier( GenericEntity::TransformChangedCaller( contained ), ApplyTransformCaller( *this ) ),
		m_contained( contained ){
		m_contained.instanceAttach( Instance::path() );

		StaticRenderableConnectionLines::instance().attach( *this );
	}
	~GenericEntityInstance(){
		StaticRenderableConnectionLines::instance().detach( *this );

		m_contained.instanceDetach( Instance::path() );
	}

	void renderSolid( Renderer& renderer, const VolumeTest& volume ) const override {
		m_contained.renderSolid( renderer, volume, Instance::localToWorld(), getSelectable().isSelected() );
	}
	void renderWireframe( Renderer& renderer, const VolumeTest& volume ) const override {
		m_contained.renderWireframe( renderer, volume, Instance::localToWorld(), getSelectable().isSelected() );
	}

	void testSelect( Selector& selector, SelectionTest& test ) override {
		m_contained.testSelect( selector, test, Instance::localToWorld() );
	}

	void evaluateTransform(){
		if ( getType() == TRANSFORM_PRIMITIVE ) {
			m_contained.translate( getTranslation() );
			if( getRotation() != c_quaternion_identity ){
				m_contained.rotate( getRotation() );
			}
		}
	}
	void applyTransform(){
		m_contained.revertTransform();
		evaluateTransform();
		m_contained.freezeTransform();
	}
	typedef MemberCaller<GenericEntityInstance, void(), &GenericEntityInstance::applyTransform> ApplyTransformCaller;
};

class GenericEntityNode final :
	public scene::Node::Symbiot,
	public scene::Instantiable,
	public scene::Cloneable
{
	class TypeCasts
	{
		NodeTypeCastTable m_casts;
	public:
		TypeCasts(){
			NodeStaticCast<GenericEntityNode, scene::Instantiable>::install( m_casts );
			NodeStaticCast<GenericEntityNode, scene::Cloneable>::install( m_casts );
			NodeContainedCast<GenericEntityNode, Snappable>::install( m_casts );
			NodeContainedCast<GenericEntityNode, TransformNode>::install( m_casts );
			NodeContainedCast<GenericEntityNode, Entity>::install( m_casts );
			NodeContainedCast<GenericEntityNode, Nameable>::install( m_casts );
			NodeContainedCast<GenericEntityNode, Namespaced>::install( m_casts );
		}
		NodeTypeCastTable& get(){
			return m_casts;
		}
	};


	InstanceSet m_instances;

	scene::Node m_node;
	GenericEntity m_contained;

public:
	typedef LazyStatic<TypeCasts> StaticTypeCasts;

	Snappable& get( NullType<Snappable> ){
		return m_contained;
	}
	TransformNode& get( NullType<TransformNode> ){
		return m_contained.getTransformNode();
	}
	Entity& get( NullType<Entity> ){
		return m_contained.getEntity();
	}
	Nameable& get( NullType<Nameable> ){
		return m_contained.getNameable();
	}
	Namespaced& get( NullType<Namespaced> ){
		return m_contained.getNamespaced();
	}

	GenericEntityNode( EntityClass* eclass ) :
		m_node( this, this, StaticTypeCasts::instance().get(), GlobalSceneGraph().currentLayer() ),
		m_contained( eclass, m_node, InstanceSet::TransformChangedCaller( m_instances ), InstanceSetEvaluateTransform<GenericEntityInstance>::Caller( m_instances ) ){
	}
	GenericEntityNode( const GenericEntityNode& other ) :
		scene::Node::Symbiot( other ),
		scene::Instantiable( other ),
		scene::Cloneable( other ),
		m_node( this, this, StaticTypeCasts::instance().get(), other.m_node.m_layer ),
		m_contained( other.m_contained, m_node, InstanceSet::TransformChangedCaller( m_instances ), InstanceSetEvaluateTransform<GenericEntityInstance>::Caller( m_instances ) ){
	}
	void release() override {
		delete this;
	}
	scene::Node& node(){
		return m_node;
	}

	scene::Node& clone() const override {
		return ( new GenericEntityNode( *this ) )->node();
	}

	scene::Instance* create( const scene::Path& path, scene::Instance* parent ) override {
		return new GenericEntityInstance( path, parent, m_contained );
	}
	void forEachInstance( const scene::Instantiable::Visitor& visitor ) override {
		m_instances.forEachInstance( visitor );
	}
	void insert( scene::Instantiable::Observer* observer, const scene::Path& path, scene::Instance* instance ) override {
		m_instances.insert( observer, path, instance );
	}
	scene::Instance* erase( scene::Instantiable::Observer* observer, const scene::Path& path ) override {
		return m_instances.erase( observer, path );
	}
};

scene::Node& New_GenericEntity( EntityClass* eclass ){
	return ( new GenericEntityNode( eclass ) )->node();
}
