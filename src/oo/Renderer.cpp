#include "Renderer.h"
#include "math/Samples.h"
#include "util/WorkQueue.h"

#include <thread>

using oo::Renderer;

std::vector<Renderer::Tile> Renderer::generateTiles(int xTileSize,
                                                    int yTileSize,
                                                    int numSamples,
                                                    int samplesPerTile) const {
  return Renderer::generateTiles(renderParams_.width, renderParams_.height,
                                 xTileSize, yTileSize, numSamples,
                                 samplesPerTile);
}

std::vector<Renderer::Tile>
Renderer::generateTiles(int width, int height, int xTileSize, int yTileSize,
                        int numSamples, int samplesPerTile) {
  std::mt19937 rng(width * height);
  std::vector<Tile> tiles;
  for (int y = 0; y < height; y += yTileSize) {
    int yBegin = y;
    int yEnd = std::min(y + yTileSize, height);
    for (int x = 0; x < width; x += xTileSize) {
      int xBegin = x;
      int xEnd = std::min(x + xTileSize, width);
      int midX = (xEnd + xBegin) / 2;
      int midY = (yEnd + yBegin) / 2;
      int centreX = width / 2;
      int centreY = height / 2;
      size_t distanceSqr = (midX - centreX) * (midX - centreX)
                           + (midY - centreY) * (midY - centreY);
      for (int s = 0; s < numSamples; s += samplesPerTile) {
        int nSamples = std::min(s + samplesPerTile, numSamples) - s;
        tiles.emplace_back(
            Tile{xBegin, xEnd, yBegin, yEnd, nSamples, s, distanceSqr, rng()});
      }
    }
  }
  std::sort(tiles.begin(), tiles.end(), [](const Tile &lhs, const Tile &rhs) {
    return lhs.key() > rhs.key();
  });
  return tiles;
}

namespace {}

static constexpr auto MaxDepth = 5;

Vec3 Renderer::radiance(std::mt19937 &rng, const Ray &ray, int depth,
                        int numUSamples, int numVSamples) const {
  if (depth >= MaxDepth)
    return Vec3();
  Primitive::IntersectionRecord intersectionRecord;
  if (!scene_.intersect(ray, intersectionRecord))
    return scene_.environment(ray);

  const auto &mat = intersectionRecord.material;
  if (renderParams_.preview)
    return mat.diffuse;
  const auto &hit = intersectionRecord.hit;

  // Create a coordinate system local to the point, where the z is the normal at
  // this point.
  const auto basis = OrthoNormalBasis::fromZ(hit.normal);

  Vec3 result;

  // TODO OO-ify materials!! Samplers?

  // Sample evenly with random offset.
  std::uniform_real_distribution<> unit(0, 1.0);
  for (auto uSample = 0; uSample < numUSamples; ++uSample) {
    for (auto vSample = 0; vSample < numVSamples; ++vSample) {
      auto u = (static_cast<double>(uSample) + unit(rng))
               / static_cast<double>(numUSamples);
      auto v = (static_cast<double>(vSample) + unit(rng))
               / static_cast<double>(numVSamples);
      auto p = unit(rng);

      // For non-"reflective" materials this should also include a reflectivity
      // term. That requires IoR of inside vs outside, and knowledge of whether
      // we're in or out. This is needed for transparency too...which we don't
      // support yet either.
      if (p < mat.reflectivity) {
        auto newRay =
            Ray(hit.position, coneSample(hit.normal.reflect(ray.direction()),
                                         mat.reflectionConeAngle(), u, v));

        // TODO should this be diffuse? seems not to me
        result +=
            mat.emission + mat.diffuse * radiance(rng, newRay, depth + 1, 1, 1);
      } else {
        // Construct the new direction.
        auto newRay = Ray(hit.position, hemisphereSample(basis, u, v));

        result +=
            mat.emission + mat.diffuse * radiance(rng, newRay, depth + 1, 1, 1);
      }
    }
  }
  if (numUSamples == 1 && numVSamples == 1)
    return result;
  else
    return result * (1.0 / (numUSamples * numVSamples));
}

// TODO: OO-ify more. Maybe hold rng as member variable, and use that as a
// non-OO Type thing? "render context" ? maybe?
// TODO: non-threaded version?
// NB I changed the sample to be centred around the pixel position + 0.5 to
// simplify the code.
ArrayOutput
Renderer::render(std::function<void(const ArrayOutput &)> updateFunc) const {
  ArrayOutput output(renderParams_.width, renderParams_.height);

  int width = renderParams_.width;
  int height = renderParams_.height;

  std::uniform_real_distribution<> unit(0.0, 1.0);
  auto renderPixel = [&](std::mt19937 &rng, int pixelX, int pixelY,
                         int numSamples) {
    Vec3 colour;
    for (int sample = 0; sample < numSamples; ++sample) {
      auto x = (pixelX + unit(rng)) / width;
      auto y = (pixelY + unit(rng)) / height;
      auto ray = camera_.ray(2 * x - 1, 2 * y - 1, rng);
      colour +=
          radiance(rng, ray, 0, FirstBounceNumUSamples, FirstBounceNumVSamples);
    }
    return colour;
  };

  WorkQueue<Tile> queue(
      generateTiles(16, 16, renderParams_.samplesPerPixel, 8));

  auto worker = [&] {
    for (;;) {
      auto tileOpt = queue.pop([&] { updateFunc(output); });
      if (!tileOpt)
        break;
      auto &tile = *tileOpt;

      std::mt19937 rng(tile.randomPrio);
      for (int y = tile.yBegin; y < tile.yEnd; ++y) {
        for (int x = tile.xBegin; x < tile.xEnd; ++x) {
          output.addSamples(x, y, renderPixel(rng, x, y, tile.samples),
                            tile.samples);
        }
      }
    }
  };
  std::vector<std::thread> threads{static_cast<size_t>(renderParams_.maxCpus)};
  std::generate(threads.begin(), threads.end(),
                [&]() { return std::thread(worker); });
  for (auto &t : threads)
    t.join();

  return output;
}
