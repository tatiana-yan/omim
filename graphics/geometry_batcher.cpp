#include "geometry_batcher.hpp"
#include "skin.hpp"
#include "color.hpp"
#include "resource_manager.hpp"
#include "skin_page.hpp"
#include "resource_style.hpp"

#include "opengl/base_texture.hpp"
#include "opengl/utils.hpp"
#include "opengl/opengl.hpp"
#include "opengl/gl_render_context.hpp"

#include "../geometry/rect2d.hpp"

#include "../base/assert.hpp"
#include "../base/math.hpp"
#include "../base/mutex.hpp"
#include "../base/logging.hpp"

#include "../std/algorithm.hpp"
#include "../std/bind.hpp"

namespace graphics
{
  GeometryBatcher::Params::Params()
    : m_useGuiResources(false)
  {}

  GeometryBatcher::GeometryBatcher(Params const & params)
    : base_t(params),
      m_isAntiAliased(true),
      m_useGuiResources(params.m_useGuiResources)
  {
    reset(-1);
    base_t::applyStates();

    /// 1 to turn antialiasing on
    /// 2 to switch it off
    m_aaShift = m_isAntiAliased ? 1 : 2;
  }

  GeometryBatcher::~GeometryBatcher()
  {
    for (size_t i = 0; i < m_pipelines.size(); ++i)
    {
      discardPipeline(i);
      freePipeline(i);
      if (m_skin->page(i)->type() != SkinPage::EStatic)
        freeTexture(i);
    }
  }

  void GeometryBatcher::reset(int pipelineID)
  {
    for (size_t i = 0; i < m_pipelines.size(); ++i)
    {
      if ((pipelineID == -1) || ((size_t)pipelineID == i))
      {
        m_pipelines[i].m_currentVertex = 0;
        m_pipelines[i].m_currentIndex = 0;
      }
    }
  }

  void GeometryBatcher::GeometryPipeline::checkStorage(shared_ptr<ResourceManager> const & resourceManager) const
  {
    if (!m_hasStorage)
    {
      if (m_useGuiResources)
        m_storage = resourceManager->guiThreadStorages()->Reserve();
      else
      {
        switch (m_type)
        {
        case SkinPage::EPrimary:
          m_storage = resourceManager->primaryStorages()->Reserve();
          break;
        case SkinPage::EFonts:
          m_storage = resourceManager->smallStorages()->Reserve();
          break;
        case SkinPage::EStatic:
          m_storage = resourceManager->smallStorages()->Reserve();
          break;
        default:
          LOG(LERROR, ("invalid storage type in checkStorage"));
        }
      }

      if (m_storage.m_vertices && m_storage.m_indices)
      {
        m_maxVertices = m_storage.m_vertices->size() / sizeof(gl::Vertex);
        m_maxIndices = m_storage.m_indices->size() / sizeof(unsigned short);

        if (!m_storage.m_vertices->isLocked())
          m_storage.m_vertices->lock();
        if (!m_storage.m_indices->isLocked())
          m_storage.m_indices->lock();

        m_vertices = (gl::Vertex*)m_storage.m_vertices->data();
        m_indices = (unsigned short *)m_storage.m_indices->data();

        m_hasStorage = true;
      }
      else
      {
        m_maxVertices = 0;
        m_maxIndices = 0;

        m_vertices = 0;
        m_indices = 0;

        m_hasStorage = false;
      }
    }
  }

  void GeometryBatcher::freePipeline(int pipelineID)
  {
    GeometryPipeline & pipeline = m_pipelines[pipelineID];

    if (pipeline.m_hasStorage)
    {
      TStoragePool * storagePool = 0;
      if (pipeline.m_useGuiResources)
        storagePool = resourceManager()->guiThreadStorages();
      else
        switch (pipeline.m_type)
        {
        case SkinPage::EPrimary:
          storagePool = resourceManager()->primaryStorages();
          break;
        case SkinPage::EFonts:
          storagePool = resourceManager()->smallStorages();
          break;
        case SkinPage::EStatic:
          storagePool = resourceManager()->smallStorages();
          break;
        default:
          LOG(LERROR, ("invalid pipeline type in freePipeline"));
          break;
        }

      base_t::freeStorage(pipeline.m_storage, storagePool);

      pipeline.m_hasStorage = false;
      pipeline.m_storage = gl::Storage();
    }
  }

  void GeometryBatcher::setSkin(shared_ptr<Skin> skin)
  {
    m_skin = skin;
    if (m_skin != 0)
    {
      /// settings proper skin page type according to useGuiResources flag
      if (m_useGuiResources)
        for (size_t i = 0; i < m_skin->pagesCount(); ++i)
          if (m_skin->page(i)->type() != SkinPage::EStatic)
            m_skin->page(i)->setType(SkinPage::ELightWeight);

      m_pipelines.resize(m_skin->pagesCount());

      m_skin->addClearPageFn(bind(&GeometryBatcher::flush, this, _1), 100);
      m_skin->addClearPageFn(bind(&GeometryBatcher::freeTexture, this, _1), 99);

      for (size_t i = 0; i < m_pipelines.size(); ++i)
      {
        m_pipelines[i].m_useGuiResources = m_useGuiResources;
        m_pipelines[i].m_currentVertex = 0;
        m_pipelines[i].m_currentIndex = 0;

        m_pipelines[i].m_hasStorage = false;
        m_pipelines[i].m_type = skin->page(i)->type();

        m_pipelines[i].m_maxVertices = 0;
        m_pipelines[i].m_maxIndices = 0;

        m_pipelines[i].m_vertices = 0;
        m_pipelines[i].m_indices = 0;
      }
    }
  }

  shared_ptr<Skin> const & GeometryBatcher::skin() const
  {
    return m_skin;
  }

  void GeometryBatcher::beginFrame()
  {
    base_t::beginFrame();
    reset(-1);
    for (size_t i = 0; i < m_pipelines.size(); ++i)
    {
      m_pipelines[i].m_verticesDrawn = 0;
      m_pipelines[i].m_indicesDrawn = 0;
    }
  }

  void GeometryBatcher::clear(graphics::Color const & c, bool clearRT, float depth, bool clearDepth)
  {
    flush(-1);
    base_t::clear(c, clearRT, depth, clearDepth);
  }

  void GeometryBatcher::setRenderTarget(shared_ptr<RenderTarget> const & rt)
  {
    flush(-1);
    base_t::setRenderTarget(rt);
  }

  void GeometryBatcher::endFrame()
  {
    flush(-1);
    /// Syncronization point.
    enableClipRect(false);

    if (isDebugging())
    {
      for (size_t i = 0; i < m_pipelines.size(); ++i)
        if ((m_pipelines[i].m_verticesDrawn != 0) || (m_pipelines[i].m_indicesDrawn != 0))
        {
          LOG(LINFO, ("pipeline #", i, " vertices=", m_pipelines[i].m_verticesDrawn, ", triangles=", m_pipelines[i].m_indicesDrawn / 3));
        }
    }

    /// is the rendering was cancelled, there possibly could
    /// be "ghost" render styles which are present in internal
    /// skin structures, but aren't rendered onto skin texture.
    /// so we are clearing the whole skin, to ensure that they
    /// are gone(slightly heavy, but very simple solution).
    if (isCancelled())
      m_skin->clearHandles();

    base_t::endFrame();
  }

  bool GeometryBatcher::hasRoom(size_t verticesCount, size_t indicesCount, int pipelineID) const
  {
    GeometryPipeline const & pipeline = m_pipelines[pipelineID];

    pipeline.checkStorage(resourceManager());
    if (!pipeline.m_hasStorage)
      return false;

    return ((pipeline.m_currentVertex + verticesCount <= pipeline.m_maxVertices)
            &&  (pipeline.m_currentIndex + indicesCount <= pipeline.m_maxIndices));
  }

  int GeometryBatcher::verticesLeft(int pipelineID) const
  {
    GeometryPipeline const & pipeline = m_pipelines[pipelineID];

    pipeline.checkStorage(resourceManager());
    if (!pipeline.m_hasStorage)
      return -1;

    return pipeline.m_maxVertices - pipeline.m_currentVertex;
  }

  int GeometryBatcher::indicesLeft(int pipelineID) const
  {
    GeometryPipeline const & pipeline = m_pipelines[pipelineID];

    pipeline.checkStorage(resourceManager());
    if (!pipeline.m_hasStorage)
      return -1;

    return pipeline.m_maxIndices - pipeline.m_currentIndex;
  }

  void GeometryBatcher::flush(int pipelineID)
  {
    if (m_skin)
    {
      for (size_t i = m_pipelines.size(); i > 0; --i)
      {
        size_t id = i - 1;

        if ((pipelineID == -1) || (id == (size_t)pipelineID))
        {
          if (flushPipeline(m_skin->page(id), id))
          {
            int nextPage = m_skin->nextPage(id);

            if (nextPage != id)
            {
              // reserving texture in advance, before we'll
              // potentially return current texture into the pool.
              m_skin->page(nextPage)->checkTexture();
            }

            m_skin->changePage(id);
          }

          /// resetting geometry storage associated
          /// with the specified pipeline.
          reset(id);
        }
      }
    }
  }

  void GeometryBatcher::freeTexture(int pipelineID)
  {
    if (!m_skin->page(pipelineID)->hasTexture())
      return;

    shared_ptr<gl::BaseTexture> texture = m_skin->page(pipelineID)->texture();
    TTexturePool * texturePool = 0;

    switch (m_skin->page(pipelineID)->type())
    {
    case SkinPage::EPrimary:
      texturePool = resourceManager()->primaryTextures();
      break;
    case SkinPage::EFonts:
      texturePool = resourceManager()->fontTextures();
      break;
    case SkinPage::ELightWeight:
      texturePool = resourceManager()->guiThreadTextures();
      break;
    case SkinPage::EStatic:
      LOG(LWARNING, ("texture with EStatic can't be freed."));
      return;
    }

    base_t::freeTexture(texture, texturePool);

    m_skin->page(pipelineID)->resetTexture();
  }

  void GeometryBatcher::unlockPipeline(int pipelineID)
  {
    GeometryPipeline & pipeline = m_pipelines[pipelineID];
    base_t::unlockStorage(pipeline.m_storage);
  }

  void GeometryBatcher::discardPipeline(int pipelineID)
  {
    GeometryPipeline & pipeline = m_pipelines[pipelineID];

    if (pipeline.m_hasStorage)
      base_t::discardStorage(pipeline.m_storage);
  }

  bool GeometryBatcher::flushPipeline(shared_ptr<SkinPage> const & skinPage,
                                      int pipelineID)
  {
    GeometryPipeline & pipeline = m_pipelines[pipelineID];
    if (pipeline.m_currentIndex)
    {
      if (skinPage->hasData())
      {
        uploadStyles(&skinPage->uploadQueue()[0], skinPage->uploadQueue().size(), skinPage->texture());
        skinPage->clearUploadQueue();
      }

      unlockPipeline(pipelineID);

      drawGeometry(skinPage->texture(),
                   pipeline.m_storage,
                   pipeline.m_currentIndex,
                   0,
                   ETriangles);

      discardPipeline(pipelineID);


      if (isDebugging())
      {
        pipeline.m_verticesDrawn += pipeline.m_currentVertex;
        pipeline.m_indicesDrawn += pipeline.m_currentIndex;
        //               LOG(LINFO, ("Pipeline #", i - 1, "draws ", pipeline.m_currentIndex / 3, "/", pipeline.m_maxIndices / 3," triangles"));
      }

      freePipeline(pipelineID);

      pipeline.m_maxIndices = 0;
      pipeline.m_maxVertices = 0;
      pipeline.m_vertices = 0;
      pipeline.m_indices = 0;
      pipeline.m_currentIndex = 0;
      pipeline.m_currentVertex = 0;

      return true;
    }

    return false;
  }

  void GeometryBatcher::drawTexturedPolygon(
      m2::PointD const & ptShift,
      ang::AngleD const & angle,
      float tx0, float ty0, float tx1, float ty1,
      float x0, float y0, float x1, float y1,
      double depth,
      int pipelineID)
  {
    if (!hasRoom(4, 6, pipelineID))
      flush(pipelineID);

    m_pipelines[pipelineID].checkStorage(resourceManager());
    if (!m_pipelines[pipelineID].m_hasStorage)
      return;

    float texMinX = tx0;
    float texMaxX = tx1;
    float texMinY = ty0;
    float texMaxY = ty1;

    shared_ptr<gl::BaseTexture> const & texture = m_skin->page(pipelineID)->texture();

    if (!texture)
    {
      LOG(LDEBUG, ("returning as no texture is reserved"));
      return;
    }

    texture->mapPixel(texMinX, texMinY);
    texture->mapPixel(texMaxX, texMaxY);

    /// rotated and translated four points (x0, y0), (x0, y1), (x1, y1), (x1, y0)

    m2::PointF coords[4] =
    {
      m2::PointF(x0 * angle.cos() - y0 * angle.sin() + ptShift.x, x0 * angle.sin() + y0 * angle.cos() + ptShift.y),
      m2::PointF(x0 * angle.cos() - y1 * angle.sin() + ptShift.x, x0 * angle.sin() + y1 * angle.cos() + ptShift.y),
      m2::PointF(x1 * angle.cos() - y1 * angle.sin() + ptShift.x, x1 * angle.sin() + y1 * angle.cos() + ptShift.y),
      m2::PointF(x1 * angle.cos() - y0 * angle.sin() + ptShift.x, x1 * angle.sin() + y0 * angle.cos() + ptShift.y)
    };

    /// Special case. Making straight fonts sharp.
    if (angle.val() == 0)
    {
      float deltaX = coords[0].x - ceil(coords[0].x);
      float deltaY = coords[0].y - ceil(coords[0].y);

      for (size_t i = 0; i < 4; ++i)
      {
        coords[i].x -= deltaX;
        coords[i].y -= deltaY;
      }
    }

    m2::PointF texCoords[4] =
    {
      m2::PointF(texMinX, texMinY),
      m2::PointF(texMinX, texMaxY),
      m2::PointF(texMaxX, texMaxY),
      m2::PointF(texMaxX, texMinY)
    };

    m2::PointF normal(0, 0);

    addTexturedFanStrided(coords, sizeof(m2::PointF),
                          &normal, 0,
                          texCoords, sizeof(m2::PointF),
                          4,
                          depth,
                          pipelineID);
  }

  void GeometryBatcher::drawStraightTexturedPolygon(
      m2::PointD const & ptPivot,
      float tx0, float ty0, float tx1, float ty1,
      float x0, float y0, float x1, float y1,
      double depth,
      int pipelineID)
  {
    if (!hasRoom(4, 6, pipelineID))
      flush(pipelineID);

    m_pipelines[pipelineID].checkStorage(resourceManager());
    if (!m_pipelines[pipelineID].m_hasStorage)
      return;

    float texMinX = tx0;
    float texMaxX = tx1;
    float texMinY = ty0;
    float texMaxY = ty1;

    shared_ptr<gl::BaseTexture> const & texture = m_skin->page(pipelineID)->texture();

    if (!texture)
    {
      LOG(LDEBUG, ("returning as no texture is reserved"));
      return;
    }

    texture->mapPixel(texMinX, texMinY);
    texture->mapPixel(texMaxX, texMaxY);

    /// rotated and translated four points (x0, y0), (x0, y1), (x1, y1), (x1, y0)

    m2::PointF offsets[4] =
    {
      m2::PointF(x0, y0),
      m2::PointF(x0, y1),
      m2::PointF(x1, y1),
      m2::PointF(x1, y0)
    };

    m2::PointF texCoords[4] =
    {
      m2::PointF(texMinX, texMinY),
      m2::PointF(texMinX, texMaxY),
      m2::PointF(texMaxX, texMaxY),
      m2::PointF(texMaxX, texMinY)
    };

    m2::PointF pv(ptPivot.x, ptPivot.y);

    addTexturedFanStrided(&pv, 0,
                          offsets, sizeof(m2::PointF),
                          texCoords, sizeof(m2::PointF),
                          4,
                          depth,
                          pipelineID);
  }


  void GeometryBatcher::addTexturedFan(m2::PointF const * coords,
                                       m2::PointF const * normals,
                                       m2::PointF const * texCoords,
                                       unsigned size,
                                       double depth,
                                       int pipelineID)
  {
    addTexturedFanStrided(coords, sizeof(m2::PointF),
                          normals, sizeof(m2::PointF),
                          texCoords, sizeof(m2::PointF),
                          size,
                          depth,
                          pipelineID);
  }

  void GeometryBatcher::addTexturedFanStrided(m2::PointF const * coords,
                                              size_t coordsStride,
                                              m2::PointF const * normals,
                                              size_t normalsStride,
                                              m2::PointF const * texCoords,
                                              size_t texCoordsStride,
                                              unsigned size,
                                              double depth,
                                              int pipelineID)
  {
    if (!hasRoom(size, (size - 2) * 3, pipelineID))
      flush(pipelineID);

    GeometryPipeline & pipeline = m_pipelines[pipelineID];

    pipeline.checkStorage(resourceManager());
    if (!pipeline.m_hasStorage)
      return;

    ASSERT(size > 2, ());

    size_t vOffset = pipeline.m_currentVertex;
    size_t iOffset = pipeline.m_currentIndex;

    for (unsigned i = 0; i < size; ++i)
    {
      pipeline.m_vertices[vOffset + i].pt = *coords;
      pipeline.m_vertices[vOffset + i].normal = *normals;
      pipeline.m_vertices[vOffset + i].tex = *texCoords;
      pipeline.m_vertices[vOffset + i].depth = depth;
      coords = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(coords) + coordsStride);
      normals = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(normals) + normalsStride);
      texCoords = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(texCoords) + texCoordsStride);
    }

    pipeline.m_currentVertex += size;

    for (size_t j = 0; j < size - 2; ++j)
    {
      pipeline.m_indices[iOffset + j * 3] = vOffset;
      pipeline.m_indices[iOffset + j * 3 + 1] = vOffset + j + 1;
      pipeline.m_indices[iOffset + j * 3 + 2] = vOffset + j + 2;
    }

    pipeline.m_currentIndex += (size - 2) * 3;
  }

  void GeometryBatcher::addTexturedStrip(
      m2::PointF const * coords,
      m2::PointF const * normals,
      m2::PointF const * texCoords,
      unsigned size,
      double depth,
      int pipelineID
      )
  {
    addTexturedStripStrided(coords, sizeof(m2::PointF),
                            normals, sizeof(m2::PointF),
                            texCoords, sizeof(m2::PointF),
                            size,
                            depth,
                            pipelineID);
  }

  void GeometryBatcher::addTexturedStripStrided(
      m2::PointF const * coords,
      size_t coordsStride,
      m2::PointF const * normals,
      size_t normalsStride,
      m2::PointF const * texCoords,
      size_t texCoordsStride,
      unsigned size,
      double depth,
      int pipelineID)
  {
    if (!hasRoom(size, (size - 2) * 3, pipelineID))
      flush(pipelineID);

    GeometryPipeline & pipeline = m_pipelines[pipelineID];

    pipeline.checkStorage(resourceManager());
    if (!pipeline.m_hasStorage)
      return;

    ASSERT(size > 2, ());

    size_t vOffset = pipeline.m_currentVertex;
    size_t iOffset = pipeline.m_currentIndex;

    for (unsigned i = 0; i < size; ++i)
    {
      pipeline.m_vertices[vOffset + i].pt = *coords;
      pipeline.m_vertices[vOffset + i].normal = *normals;
      pipeline.m_vertices[vOffset + i].tex = *texCoords;
      pipeline.m_vertices[vOffset + i].depth = depth;
      coords = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(coords) + coordsStride);
      normals = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(normals) + normalsStride);
      texCoords = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(texCoords) + texCoordsStride);
    }

    pipeline.m_currentVertex += size;

    size_t oldIdx1 = vOffset;
    size_t oldIdx2 = vOffset + 1;

    for (size_t j = 0; j < size - 2; ++j)
    {
      pipeline.m_indices[iOffset + j * 3] = oldIdx1;
      pipeline.m_indices[iOffset + j * 3 + 1] = oldIdx2;
      pipeline.m_indices[iOffset + j * 3 + 2] = vOffset + j + 2;

      oldIdx1 = oldIdx2;
      oldIdx2 = vOffset + j + 2;
    }

    pipeline.m_currentIndex += (size - 2) * 3;
  }

  void GeometryBatcher::addTexturedListStrided(
      m2::PointD const * coords,
      size_t coordsStride,
      m2::PointF const * normals,
      size_t normalsStride,
      m2::PointF const * texCoords,
      size_t texCoordsStride,
      unsigned size,
      double depth,
      int pipelineID)
  {
    if (!hasRoom(size, size, pipelineID))
      flush(pipelineID);

    GeometryPipeline & pipeline = m_pipelines[pipelineID];

    pipeline.checkStorage(resourceManager());
    if (!pipeline.m_hasStorage)
      return;

    ASSERT(size > 2, ());

    size_t vOffset = pipeline.m_currentVertex;
    size_t iOffset = pipeline.m_currentIndex;

    for (size_t i = 0; i < size; ++i)
    {
      pipeline.m_vertices[vOffset + i].pt = m2::PointF(coords->x, coords->y);
      pipeline.m_vertices[vOffset + i].normal = *normals;
      pipeline.m_vertices[vOffset + i].tex = *texCoords;
      pipeline.m_vertices[vOffset + i].depth = depth;
      coords = reinterpret_cast<m2::PointD const*>(reinterpret_cast<unsigned char const*>(coords) + coordsStride);
      normals = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(normals) + normalsStride);
      texCoords = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(texCoords) + texCoordsStride);
    }

    pipeline.m_currentVertex += size;

    for (size_t i = 0; i < size; ++i)
      pipeline.m_indices[iOffset + i] = vOffset + i;

    pipeline.m_currentIndex += size;
  }


  void GeometryBatcher::addTexturedListStrided(
      m2::PointF const * coords,
      size_t coordsStride,
      m2::PointF const * normals,
      size_t normalsStride,
      m2::PointF const * texCoords,
      size_t texCoordsStride,
      unsigned size,
      double depth,
      int pipelineID)
  {
    if (!hasRoom(size, size, pipelineID))
      flush(pipelineID);

    GeometryPipeline & pipeline = m_pipelines[pipelineID];

    pipeline.checkStorage(resourceManager());
    if (!pipeline.m_hasStorage)
      return;

    ASSERT(size > 2, ());

    size_t vOffset = pipeline.m_currentVertex;
    size_t iOffset = pipeline.m_currentIndex;

    for (size_t i = 0; i < size; ++i)
    {
      pipeline.m_vertices[vOffset + i].pt = *coords;
      pipeline.m_vertices[vOffset + i].normal = *normals;
      pipeline.m_vertices[vOffset + i].tex = *texCoords;
      pipeline.m_vertices[vOffset + i].depth = depth;
      coords = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(coords) + coordsStride);
      normals = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(normals) + normalsStride);
      texCoords = reinterpret_cast<m2::PointF const*>(reinterpret_cast<unsigned char const*>(texCoords) + texCoordsStride);
    }

    pipeline.m_currentVertex += size;

    for (size_t i = 0; i < size; ++i)
      pipeline.m_indices[iOffset + i] = vOffset + i;

    pipeline.m_currentIndex += size;
  }

  void GeometryBatcher::addTexturedList(m2::PointF const * coords,
                                        m2::PointF const * normals,
                                        m2::PointF const * texCoords,
                                        unsigned size,
                                        double depth,
                                        int pipelineID)
  {
    addTexturedListStrided(coords, sizeof(m2::PointF),
                           normals, sizeof(m2::PointF),
                           texCoords, sizeof(m2::PointF),
                           size, depth, pipelineID);
  }

  void GeometryBatcher::enableClipRect(bool flag)
  {
    flush(-1);
    base_t::enableClipRect(flag);
  }

  void GeometryBatcher::setClipRect(m2::RectI const & rect)
  {
    flush(-1);
    base_t::setClipRect(rect);
  }

  int GeometryBatcher::aaShift() const
  {
    return m_aaShift;
  }

  void GeometryBatcher::memoryWarning()
  {
    if (m_skin)
      m_skin->memoryWarning();
  }

  void GeometryBatcher::enterBackground()
  {
    if (m_skin)
      m_skin->enterBackground();
  }

  void GeometryBatcher::enterForeground()
  {
    if (m_skin)
      m_skin->enterForeground();
  }

  void GeometryBatcher::setDisplayList(DisplayList * dl)
  {
    flush(-1);
    base_t::setDisplayList(dl);
  }

  void GeometryBatcher::drawDisplayList(DisplayList * dl, math::Matrix<double, 3, 3> const & m)
  {
    flush(-1);
    base_t::drawDisplayList(dl, m);
  }

  void GeometryBatcher::uploadStyles(shared_ptr<ResourceStyle> const * styles,
                                     size_t count,
                                     shared_ptr<gl::BaseTexture> const & texture)
  {
    /// splitting the whole queue of commands into the chunks no more
    /// than 64KB of uploadable data each

    size_t bytesUploaded = 0;
    size_t bytesPerPixel = graphics::formatSize(resourceManager()->params().m_texFormat);
    size_t prev = 0;

    for (size_t i = 0; i < count; ++i)
    {
      shared_ptr<ResourceStyle> const & style = styles[i];

      bytesUploaded += style->m_texRect.SizeX() * style->m_texRect.SizeY() * bytesPerPixel;

      if (bytesUploaded > 64 * 1024)
      {
        base_t::uploadStyles(styles + prev, i + 1 - prev, texture);
        if (i + 1 < count)
          addCheckPoint();

        prev = i + 1;
        bytesUploaded = 0;
      }
    }

    if (count != 0)
    {
      base_t::uploadStyles(styles, count, texture);
      bytesUploaded = 0;
    }
  }

  void GeometryBatcher::applyStates()
  {
    flush(-1);
    base_t::applyStates();
  }

  void GeometryBatcher::applyBlitStates()
  {
    flush(-1);
    base_t::applyBlitStates();
  }

  void GeometryBatcher::applySharpStates()
  {
    flush(-1);
    base_t::applySharpStates();
  }
} // namespace graphics
